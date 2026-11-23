#include "character_math.hpp"
#include "algorithm_support.hpp"
#include <array>
#include <map>
#include <vector>

namespace equipment_calculator {
namespace {
using namespace algorithm_detail;
using Stats = std::map<std::string, double, std::less<>>;
double Value(const Stats& values, std::string_view id) {
    const auto found = values.find(id); return found == values.end() ? 0 : found->second;
}
void Add(Stats& target, const std::string& id, double value) {
    target[id] += value;
    if (!std::isfinite(target[id])) Fail(AlgorithmFailure::invalid_inventory);
}
Stats ReadStats(const Json& input) {
    if (!input.is_object()) Fail(AlgorithmFailure::incomplete_attributes);
    Stats result;
    for (const auto& [id, value] : input.items()) {
        if (id.empty() || id.size() > 128) Fail();
        result.emplace(id, Number(value, AlgorithmFailure::incomplete_attributes));
    }
    return result;
}
void AttributeMetadata(const Json& input, Json& result) {
    for (const auto* key : {"attributes_metadata", "attributes_metadata_source", "attributes_metadata_scope"})
        if (input.contains(key)) result[key] = input.at(key);
}
struct Panel { Stats raw; Json sources = Json::array(); };
Panel BuildPanel(const Json& request, Budget& budget) {
    if (!request.contains("sources") || !request.at("sources").is_array() || request.at("sources").empty())
        Fail(AlgorithmFailure::incomplete_attributes);
    Panel panel; std::set<std::string> sources;
    for (const auto& source : request.at("sources")) {
        if (budget.Stop()) Fail(AlgorithmFailure::budget_exhausted);
        const auto id = Id(source.at("id"));
        if (!sources.insert(id).second || !source.value("complete", false) || !source.value("unconditional", true) ||
            !source.value("operations_complete", true)) Fail(AlgorithmFailure::incomplete_attributes);
        auto stats = source.contains("attributes") ? ReadStats(source.at("attributes")) : Stats{};
        if (!source.contains("attributes") && !source.contains("modifiers")) Fail(AlgorithmFailure::incomplete_attributes);
        for (const auto& [property, value] : stats) Add(panel.raw, property, value);
        Json detail = source;
        if (source.contains("modifiers")) {
            if (!source.at("modifiers").is_array()) Fail(AlgorithmFailure::incomplete_attributes);
            auto modifiers = source.at("modifiers");
            const auto order = [](const Json& modifier) { return modifier.contains("sort") ? Integer(modifier.at("sort"), INT32_MIN, INT32_MAX) : 0; };
            std::stable_sort(modifiers.begin(), modifiers.end(), [&](const Json& a, const Json& b) { return order(a) < order(b); });
            for (const auto& modifier : modifiers) {
                const auto property = Id(modifier.at("id"));
                const auto value = Number(modifier.at("value"), AlgorithmFailure::incomplete_attributes);
                if (!modifier.contains("operation") || !modifier.at("operation").is_string()) Fail(AlgorithmFailure::incomplete_attributes);
                const auto operation = modifier.at("operation").get<std::string>(); const auto previous = Value(panel.raw, property);
                if (operation == "additive") Add(panel.raw, property, value);
                else if (operation == "decrement") Add(panel.raw, property, -value);
                else if (operation == "override") panel.raw[property] = value;
                else Fail(AlgorithmFailure::incomplete_attributes);
                Add(stats, property, Value(panel.raw, property) - previous);
            }
            detail["modifiers"] = std::move(modifiers);
        }
        detail["attributes"] = stats;
        panel.sources.push_back(std::move(detail)); ++budget.nodes;
    }
    Stats equipment;
    if (request.contains("equipment")) {
        if (sources.contains("equipment")) Fail();
        if (!request.at("equipment").is_array()) Fail();
        for (const auto& item : request.at("equipment")) {
            if (!item.value("main_stats_complete", false) || !item.value("sub_stats_values_complete", false))
                Fail(AlgorithmFailure::incomplete_attributes);
            const auto add = [&](const Json& values, bool main) {
                if (!values.is_array()) Fail(AlgorithmFailure::incomplete_attributes);
                for (const auto& row : values) Add(equipment, Id(row.at("id")),
                    Number(row.at(main && row.contains("max_value") ? "max_value" : "value"), AlgorithmFailure::incomplete_attributes));
            };
            if (item.value("kind", std::string{}) == "core") {
                if (!item.contains("main_stat")) Fail(AlgorithmFailure::incomplete_main_stat);
                add(Json::array({item.at("main_stat")}), true);
            } else add(item.at("main_stats"), true);
            add(item.at("sub_stats"), false);
        }
        panel.sources.push_back({{"id", "equipment"}, {"attributes", equipment}});
        sources.insert("equipment");
        for (const auto& [id, value] : equipment) Add(panel.raw, id, value);
    }
    if (request.contains("extra_shape")) {
        if (sources.contains("extra_shape")) Fail();
        const auto& extra = request.at("extra_shape");
        const auto area = Integer(extra.at("area"), 1, 4); const auto buffs = ReadStats(extra.at("buffs"));
        std::size_t count{};
        if (!request.contains("equipment")) Fail(AlgorithmFailure::incomplete_attributes);
        for (const auto& item : request.at("equipment")) if (item.value("kind", std::string{}) == "module" &&
            item.contains("cells") && item.at("cells").size() == static_cast<std::size_t>(area)) ++count;
        Stats contribution;
        for (const auto& [id, value] : buffs) { contribution[id] = value * static_cast<double>(count); Add(panel.raw, id, contribution[id]); }
        panel.sources.push_back({{"id", "extra_shape"}, {"attributes", contribution}, {"count", count}});
        sources.insert("extra_shape");
    }
    if (request.contains("required_sources")) {
        if (!request.at("required_sources").is_array()) Fail();
        for (const auto& id : request.at("required_sources")) if (!sources.contains(Id(id))) Fail(AlgorithmFailure::incomplete_attributes);
    }
    for (const auto* id : {"AtkBase", "HPMaxBase", "DefBase", "CritBase", "CritDamageBase"})
        if (!panel.raw.contains(id)) Fail(AlgorithmFailure::incomplete_attributes);
    return panel;
}

double SumArray(const Json& object, const char* key) {
    if (!object.contains(key)) return 0;
    if (!object.at(key).is_array()) Fail();
    double sum{}; for (const auto& value : object.at(key)) sum += Number(value);
    if (!std::isfinite(sum)) Fail(); return sum;
}
Json Direct(const Stats& stats, const Json& request) {
    if (!request.contains("damage") || !request.at("damage").is_object()) Fail(AlgorithmFailure::incomplete_attributes);
    const auto& model = request.at("damage");
    const auto property = Id(model.at("property"));
    constexpr std::array<std::string_view, 7> elements{"DamageUpChaosBase", "DamageUpCosmosBase", "DamageUpIncantationBase",
        "DamageUpLakshanaBase", "DamageUpNatureBase", "DamageUpPsycheBase", "DamageUpPsychicallyBase"};
    if (std::find(elements.begin(), elements.end(), property) == elements.end()) Fail();
    const auto level = Number(request.at("character_level")); if (level <= 0) Fail();
    if (!model.contains("enemy") || !model.at("enemy").is_object()) Fail(AlgorithmFailure::incomplete_attributes);
    const auto& enemy = model.at("enemy");
    const auto penetration = Value(stats, "DefIgnore");
    const auto reduction = enemy.contains("defense_reduction") ? Number(enemy.at("defense_reduction")) : 0;
    double defense{};
    if (enemy.contains("defense_base")) {
        const auto base = Number(enemy.at("defense_base")); if (base < 0) Fail();
        const auto up = enemy.contains("defense_up") ? Number(enemy.at("defense_up")) : 0;
        const auto flat = enemy.contains("defense_add") ? Number(enemy.at("defense_add")) : 0;
        defense = (base * (1 + up) + flat) / 6 * (1 - penetration) * (1 - reduction);
    } else {
        const auto enemy_level = Number(enemy.at("level")); if (enemy_level < 0) Fail();
        const auto scene = Id(enemy.at("scene"));
        const double offset = scene == "outer_realm" ? 90 : scene == "open_world" ? 100 : 0;
        if (!offset) Fail();
        defense = (enemy_level + offset) * (1 - penetration) * (1 - reduction);
    }
    const auto denominator = defense + level + 100;
    if (denominator <= 0) Fail();
    const auto resistance = Number(enemy.at("resistance")) - SumArray(enemy, "resistance_reductions") - SumArray(enemy, "resistance_penetrations");
    const auto resistance_factor = resistance >= 0 ? 1 - resistance : 1 - resistance / 1.10;
    const auto attack = Value(stats, "AtkBase") * (1 + Value(stats, "AtkUp")) + Value(stats, "AtkAdd");
    auto crit_rate = Value(stats, "CritBase") + Value(stats, "CritAdd");
    if (model.contains("crit_cap") && !model.at("crit_cap").is_null()) {
        const auto cap = Number(model.at("crit_cap")); if (cap < 0 || cap > 1) Fail();
        crit_rate = std::clamp(crit_rate, 0., cap);
    }
    const auto crit_damage = Value(stats, "CritDamageBase") + Value(stats, "CritDamageAdd");
    double independent{1};
    if (model.contains("independent_bonuses")) {
        if (!model.at("independent_bonuses").is_array()) Fail();
        for (const auto& value : model.at("independent_bonuses")) independent *= 1 + Number(value);
    }
    const auto bonus = 1 + Value(stats, "DamageUpGeneralBase") + Value(stats, "DamageUpGeneralAdd") + Value(stats, property);
    const auto vulnerability = 1 + SumArray(enemy, "vulnerability_increases");
    const auto defense_factor = (level + 100) / denominator;
    const auto noncritical = attack * bonus * defense_factor * resistance_factor * vulnerability * independent;
    const auto expected = noncritical * (1 + crit_rate * crit_damage);
    if (!std::isfinite(expected) || expected < 0) Fail(AlgorithmFailure::invalid_inventory);
    return {{"value", expected}, {"non_critical", noncritical}, {"critical", noncritical * (1 + crit_damage)},
        {"crit_rate", crit_rate}, {"crit_damage", crit_damage}, {"enemy_defense", defense}, {"effective_resistance", resistance},
        {"factors", {{"skill_multiplier", 1.0}, {"attack", attack}, {"damage_increase", bonus}, {"critical", 1 + crit_rate * crit_damage},
            {"defense", defense_factor}, {"resistance", resistance_factor}, {"vulnerability", vulnerability}, {"independent", independent}}}};
}

Json GraduationTemplate(const Json& graduation, const Json& baseline, Budget& budget) {
    const auto& catalog = graduation.at("catalog");
    const auto weights = ReadStats(graduation.at("weights"));
    const auto main_weights = graduation.contains("main_weights") ? ReadStats(graduation.at("main_weights")) : weights;
    const auto per_cell = ReadStats(catalog.at("gold_sub_values_per_cell"));
    const auto core_sub = ReadStats(catalog.at("core_sub_values"));
    const auto core_main = ReadStats(catalog.at("core_main_values"));
    const auto area = Integer(catalog.at("module_area"), 1, 25);
    std::vector<std::string> properties;
    for (const auto& [id, value] : per_cell) if (value > 0 && core_sub.contains(id)) properties.push_back(id);
    std::stable_sort(properties.begin(), properties.end(), [&](const auto& a, const auto& b) {
        const auto left = Value(weights, a), right = Value(weights, b); return left == right ? a < b : left > right;
    });
    if (properties.size() < 4) Fail(AlgorithmFailure::incomplete_attributes);
    properties.resize(4);
    Json module_sub = Json::array(), tape_sub = Json::array();
    for (const auto& id : properties) {
        module_sub.push_back({{"id", id}, {"value", per_cell.at(id) * area}});
        tape_sub.push_back({{"id", id}, {"value", core_sub.at(id)}});
    }
    if (catalog.contains("extra_buffs")) {
        const auto count = Integer(catalog.at("extra_count"), 0, 25);
        for (const auto& [id, value] : ReadStats(catalog.at("extra_buffs")))
            module_sub.push_back({{"id", id}, {"value", value * static_cast<double>(count)}});
    }
    Json aggregate{{"kind", "module"}, {"main_stats_complete", true}, {"sub_stats_values_complete", true},
        {"main_stats", catalog.at("module_main_stats")}, {"sub_stats", module_sub}};
    Json core{{"kind", "core"}, {"main_stats_complete", true}, {"sub_stats_values_complete", true}, {"sub_stats", tape_sub}};
    std::vector<std::string> candidates;
    for (const auto& [id, value] : core_main) if (value > 0 && Value(main_weights, id) > 0) candidates.push_back(id);
    std::sort(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) {
        const auto left = Value(main_weights, a), right = Value(main_weights, b); return left == right ? a < b : left > right;
    });
    if (candidates.empty() && catalog.contains("default_core_main")) candidates.push_back(Id(catalog.at("default_core_main")));
    if (candidates.empty()) Fail(AlgorithmFailure::incomplete_main_stat);
    Json best; double best_damage{-1};
    for (const auto& id : candidates) {
        if (budget.Stop()) Fail(AlgorithmFailure::budget_exhausted);
        if (!core_main.contains(id)) Fail(AlgorithmFailure::incomplete_main_stat);
        core["main_stat"] = {{"id", id}, {"value", core_main.at(id)}, {"max_value", core_main.at(id)}};
        auto request = baseline; request["equipment"] = Json::array({aggregate, core}); request.erase("extra_shape");
        const auto panel = BuildPanel(request, budget); const auto damage = Direct(panel.raw, request).at("value").get<double>();
        if (damage > best_damage) { best_damage = damage; best = {{"equipment", request.at("equipment")}, {"selected_sub_stats", properties}, {"selected_main", id}}; }
    }
    best["native_assemblable"] = false; return best;
}

Json Character(const Json& request, Budget& budget) {
    if (!request.is_object()) Fail();
    auto panel = BuildPanel(request, budget);
    Json derived = Json::object();
    for (const auto& [name, base, up, flat] : std::array{
        std::array{"Atk", "AtkBase", "AtkUp", "AtkAdd"}, std::array{"HPMax", "HPMaxBase", "HPMaxUp", "HPMaxAdd"},
        std::array{"Def", "DefBase", "DefUp", "DefAdd"}})
        {
            const auto value = Value(panel.raw, base) * (1 + Value(panel.raw, up)) + Value(panel.raw, flat);
            if (!std::isfinite(value)) Fail(AlgorithmFailure::invalid_inventory);
            derived[name] = value;
        }
    derived["CritRate"] = Value(panel.raw, "CritBase") + Value(panel.raw, "CritAdd");
    derived["CritDamage"] = Value(panel.raw, "CritDamageBase") + Value(panel.raw, "CritDamageAdd");
    Json result{{"available", true}, {"attributes", panel.raw}, {"derived", derived}, {"sources", panel.sources}};
    AttributeMetadata(request, result);
    if (request.contains("damage")) {
        const auto damage = Direct(panel.raw, request); const auto baseline = damage.at("value").get<double>();
        result["direct_damage"] = damage;
        result["marginal_available"] = baseline > 0;
        Json margins = Json::array();
        std::map<std::string, double, std::less<>> units{{"CritBase", .01}, {"CritDamageBase", .02}, {"DamageUpGeneralBase", .01}, {"AtkUp", .0125}, {"AtkAdd", 8}};
        units.emplace(Id(request.at("damage").at("property")), .0125);
        if (request.contains("benefit_units")) for (const auto& [id, value] : ReadStats(request.at("benefit_units"))) {
            if (!units.contains(id)) Fail(); units[id] = value;
        }
        auto weights = request.contains("base_weights") ? ReadStats(request.at("base_weights")) : Stats{};
        auto main_weights = request.contains("base_main_weights") ? ReadStats(request.at("base_main_weights")) : weights;
        double largest{};
        for (const auto& [id, unit] : units) if (baseline > 0) {
            if (budget.Stop()) Fail(AlgorithmFailure::budget_exhausted);
            if (unit <= 0) Fail();
            auto next = panel.raw; Add(next, id, unit);
            const auto next_damage = Direct(next, request).at("value").get<double>();
            const auto gain = baseline > 0 ? (next_damage / baseline - 1) * 100 : 0;
            if (!std::isfinite(gain)) Fail(AlgorithmFailure::invalid_inventory);
            largest = std::max(largest, gain);
            auto current_value = Value(panel.raw, id);
            if (id == "CritBase") current_value += Value(panel.raw, "CritAdd");
            else if (id == "CritDamageBase") current_value += Value(panel.raw, "CritDamageAdd");
            else if (id == "DamageUpGeneralBase") current_value += Value(panel.raw, "DamageUpGeneralAdd");
            margins.push_back({{"id", id}, {"current_value", current_value}, {"unit", unit}, {"next_damage", next_damage}, {"gain_percent", gain}});
            ++budget.nodes;
        }
        for (const auto& row : margins) {
            const auto normalized = largest > 0 ? std::max(0., row.at("gain_percent").get<double>()) / largest : 0;
            weights[row.at("id").get<std::string>()] = normalized;
            main_weights[row.at("id").get<std::string>()] = normalized;
        }
        std::stable_sort(margins.begin(), margins.end(), [](const Json& a, const Json& b) { return a.at("gain_percent") > b.at("gain_percent"); });
        result["margins"] = margins; result["dynamic_weights"] = weights; result["dynamic_main_weights"] = main_weights;
        if (request.contains("graduation")) {
            const auto& graduation = request.at("graduation");
            if (!graduation.contains("baseline") || (!graduation.contains("benchmark") && !graduation.contains("catalog"))) Fail(AlgorithmFailure::incomplete_attributes);
            // Both sides are explicitly frozen at the same maximum progression.
            // The benchmark differs only in equipment and cannot silently use
            // the current character's lower progression on the numerator.
            auto actual_request = graduation.at("baseline");
            auto benchmark_request = actual_request;
            Json template_info;
            if (graduation.contains("benchmark")) benchmark_request["equipment"] = graduation.at("benchmark");
            else {
                template_info = GraduationTemplate(graduation, actual_request, budget);
                benchmark_request["equipment"] = template_info.at("equipment"); benchmark_request.erase("extra_shape");
            }
            if (graduation.contains("benchmark_extra_shape")) benchmark_request["extra_shape"] = graduation.at("benchmark_extra_shape");
            auto actual_panel = BuildPanel(actual_request, budget), benchmark_panel = BuildPanel(benchmark_request, budget);
            const auto actual = Direct(actual_panel.raw, actual_request).at("value").get<double>();
            const auto reference = Direct(benchmark_panel.raw, benchmark_request).at("value").get<double>();
            if (reference > 0 && !std::isfinite(actual / reference)) Fail(AlgorithmFailure::invalid_inventory);
            result["graduation"] = {{"available", reference > 0}, {"actual_damage", actual}, {"benchmark_damage", reference},
                {"rate", reference > 0 ? Json(actual / reference) : Json(nullptr)}};
            if (!template_info.is_null()) result["graduation"]["template"] = std::move(template_info);
        }
    }
    result["search"] = budget.Report(); return result;
}

Json Gain(double damage, double baseline) {
    Json value = nullptr;
    if (baseline > 0) {
        const auto gain = (damage / baseline - 1) * 100;
        if (!std::isfinite(gain)) Fail(AlgorithmFailure::invalid_inventory);
        value = gain;
    }
    return {{"available", baseline > 0}, {"damage", damage}, {"baseline_damage", baseline}, {"gain_percent", value}};
}
double PanelDamage(const Json& model, Budget& budget) { return Direct(BuildPanel(model, budget).raw, model).at("value").get<double>(); }
std::set<std::uint64_t> EquipmentIds(const Json& model) {
    if (!model.contains("equipment") || !model.at("equipment").is_array()) Fail(AlgorithmFailure::incomplete_attributes);
    std::set<std::uint64_t> result;
    for (const auto& item : model.at("equipment")) if (!result.insert(Key(item.at("uid"))).second) Fail(AlgorithmFailure::invalid_inventory);
    return result;
}
Json Contributions(const Json& request, Budget& budget) {
    static_cast<void>(EquipmentIds(request));
    const auto damage = PanelDamage(request, budget);
    auto empty = request; empty["equipment"] = Json::array();
    Json result{{"equipment_gain", Gain(damage, PanelDamage(empty, budget))}, {"items", Json::array()}};
    for (const auto& item : request.at("equipment")) {
        if (budget.Stop()) Fail(AlgorithmFailure::budget_exhausted);
        const auto key = Key(item.at("uid")); auto remaining = request; remaining["equipment"] = Json::array();
        for (const auto& other : request.at("equipment")) if (Key(other.at("uid")) != key) remaining["equipment"].push_back(other);
        auto gain = Gain(damage, PanelDamage(remaining, budget)); gain["equipment"] = item.at("uid"); result["items"].push_back(std::move(gain));
    }
    if (request.contains("snapshot_id")) result["source_snapshot_id"] = request.at("snapshot_id");
    AttributeMetadata(request, result);
    result["search"] = budget.Report(); return result;
}
Json Replacements(const Json& request, const Json& inventory, Budget& budget) {
    Snapshot(request, inventory);
    const auto target_id = Key(request.at("equipment"));
    auto model = request.at("model"); auto selected = EquipmentIds(model);
    if (!selected.contains(target_id)) Fail();
    std::map<std::uint64_t, const Json*> items;
    for (const auto& item : inventory.at("items")) if (!items.emplace(Key(item.at("uid")), &item).second) Fail(AlgorithmFailure::invalid_inventory);
    // Attributes used for both current and replacement items come from the
    // same snapshot. The caller supplies only the normalized permanent model.
    for (auto& item : model["equipment"]) {
        const auto found = items.find(Key(item.at("uid")));
        if (found == items.end()) Fail(AlgorithmFailure::invalid_inventory);
        item = *found->second;
    }
    const auto& target = *items.at(target_id); const auto kind = Id(target.at("kind"));
    if (kind != "module" && kind != "core") Fail(AlgorithmFailure::invalid_inventory);
    if (kind == "module" && (!target.contains("shape_id") || !target.contains("cells") || !target.at("cells").is_array() || target.at("cells").empty()))
        Fail(AlgorithmFailure::incomplete_shape);
    if (kind == "core" && (!target.contains("suit_id") || !target.contains("required_shapes") || !target.at("required_shapes").is_array()))
        Fail(AlgorithmFailure::incomplete_set);
    if (request.contains("weights")) model["base_weights"] = request.at("weights");
    if (request.contains("main_weights")) model["base_main_weights"] = request.at("main_weights");
    model.erase("graduation");
    const auto panel = Character(model, budget);
    if (!panel.contains("direct_damage")) Fail(AlgorithmFailure::incomplete_attributes);
    const auto baseline = panel.at("direct_damage").at("value").get<double>();
    const Json preferences{{"weights", panel.at("dynamic_weights")}, {"main_weights", panel.at("dynamic_main_weights")}};
    const auto current_score = ScoreEquipment(target, preferences);
    auto remaining = model; remaining["equipment"] = Json::array();
    for (const auto& item : model.at("equipment")) if (Key(item.at("uid")) != target_id) remaining["equipment"].push_back(item);
    const auto without = PanelDamage(remaining, budget);
    std::set<std::uint64_t> excluded;
    if (request.contains("exclude_uids")) {
        if (!request.at("exclude_uids").is_array()) Fail();
        for (const auto& uid : request.at("exclude_uids")) excluded.insert(Key(uid));
    }
    const bool borrow = request.value("allow_borrow", true), discarded = request.value("include_discarded", false);
    const auto limit = request.contains("limit") ? Integer(request.at("limit"), 1, 500) : 200;
    Json rows = Json::array(), errors = Json::array(); bool complete{true};
    auto required = kind == "core" ? target.at("required_shapes") : Json::array(); std::sort(required.begin(), required.end());
    for (const auto& item : inventory.at("items")) {
        if (budget.Stop()) { complete = false; break; }
        const auto uid = Key(item.at("uid"));
        if (selected.contains(uid) || excluded.contains(uid) || item.at("kind") != kind || (!discarded && item.value("discarded", false))) continue;
        if (!borrow && item.contains("character") && !item.at("character").is_null() &&
            (!request.contains("character") || request.at("character").is_null() || Key(item.at("character")) != Key(request.at("character")))) continue;
        try {
            if (kind == "module") {
                if (item.value("shape_id", std::string{}) != target.at("shape_id") || item.value("cells", Json(nullptr)) != target.at("cells")) continue;
            } else {
                if (item.value("suit_id", std::string{}) != target.at("suit_id")) continue;
                if (!item.contains("required_shapes") || !item.at("required_shapes").is_array()) Fail(AlgorithmFailure::incomplete_set);
                auto shapes = item.at("required_shapes"); std::sort(shapes.begin(), shapes.end()); if (shapes != required) continue;
            }
            const auto score = ScoreEquipment(item, preferences);
            auto replaced = model;
            for (auto& current : replaced["equipment"]) if (Key(current.at("uid")) == target_id) current = item;
            const auto damage = PanelDamage(replaced, budget);
            rows.push_back({{"equipment", item.at("uid")}, {"score", score.at("score")}, {"grade", score.at("grade")},
                {"damage", damage}, {"gain_percent", Gain(damage, baseline).at("gain_percent")},
                {"direct_damage_score", Gain(damage, without).at("gain_percent")}});
        } catch (const InvalidAlgorithm& failure) {
            if (failure.reason == AlgorithmFailure::cancelled) throw;
            complete = false;
            if (failure.reason == AlgorithmFailure::budget_exhausted) break;
            errors.push_back({{"equipment", item.at("uid")}, {"error_code", AlgorithmErrorCode(failure.reason)}});
        }
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Json& a, const Json& b) {
        if (a.at("score") != b.at("score")) return a.at("score") > b.at("score");
        if (a.at("damage") != b.at("damage")) return a.at("damage") > b.at("damage");
        return Key(a.at("equipment")) < Key(b.at("equipment"));
    });
    const auto total = rows.size(); if (rows.size() > static_cast<std::size_t>(limit)) rows.erase(rows.begin() + limit, rows.end());
    Json result{{"source_snapshot_id", request.at("snapshot_id")}, {"equipment", request.at("equipment")}, {"complete", complete},
        {"baseline_damage", baseline}, {"without_item_damage", without}, {"current_score", current_score.at("score")},
        {"current_direct_damage_score", Gain(baseline, without).at("gain_percent")},
        {"dynamic_weights", panel.at("dynamic_weights")}, {"dynamic_main_weights", panel.at("dynamic_main_weights")},
        {"candidates", rows}, {"errors", errors}, {"total", total}, {"truncated", rows.size() != total}, {"search", budget.Report()}};
    AttributeMetadata(model, result); return result;
}
} // namespace
Json CalculateCharacter(const Json& request, const AlgorithmCancelCheck& cancel_check) {
    try { Budget budget(request, cancel_check); return Character(request, budget); } catch (const Json::exception&) { Fail(); }
}
Json CalculateEquipmentContributions(const Json& request, const AlgorithmCancelCheck& cancel_check) {
    try { Budget budget(request, cancel_check); return Contributions(request, budget); } catch (const Json::exception&) { Fail(); }
}
Json RankEquipmentReplacements(const Json& request, const Json& frozen_inventory, const AlgorithmCancelCheck& cancel_check) {
    try { Budget budget(request, cancel_check); return Replacements(request, frozen_inventory, budget); } catch (const Json::exception&) { Fail(); }
}
} // namespace equipment_calculator
