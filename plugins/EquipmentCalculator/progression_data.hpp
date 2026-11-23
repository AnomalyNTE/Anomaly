#pragma once

#include "model_data.hpp"
#include <charconv>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace equipment_calculator::progression_data {
using Json = nlohmann::json;
using Quantity = std::int64_t;
using Stock = std::map<std::string, Quantity, std::less<>>;

inline std::string_view Trim(std::string_view text) {
    constexpr std::string_view spaces = " \t\r\n";
    const auto first = text.find_first_not_of(spaces);
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(spaces) - first + 1);
}
inline std::optional<Quantity> Count(std::string_view text) {
    text = Trim(text); if (text.empty()) return std::nullopt; Quantity result{};
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), result);
    if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size() || result < 0 || result > 1000000000000000LL) return std::nullopt;
    return result;
}
inline bool Add(Stock& values, std::string id, Quantity amount) {
    auto& current = values[std::move(id)];
    if (amount < 0 || amount > 1000000000000000LL - current) return false;
    current += amount; return true;
}
inline std::string Canonical(std::string_view id) {
    // This exact token is the game's progression currency identifier.
    return id == "gold" ? "Gold" : std::string(id);
}
inline std::optional<Stock> ParseCosts(std::string_view text) {
    Stock result; text = Trim(text);
    if (text.empty() || text == "0") return result;
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto entry = Trim(text.substr(0, comma));
        const auto colon = entry.find(':');
        if (colon == std::string_view::npos || entry.find(':', colon + 1) != std::string_view::npos) return std::nullopt;
        const auto id = Trim(entry.substr(0, colon));
        const auto amount = Count(entry.substr(colon + 1));
        if (id.empty() || !amount || !*amount || !Add(result, Canonical(id), *amount)) return std::nullopt;
        if (comma == std::string_view::npos) break;
        text = Trim(text.substr(comma + 1));
        if (text.empty()) return std::nullopt;
    }
    return result;
}
inline Json Track(std::string_view id, const Json& from, const Json& to, const char* key, std::int32_t minimum = 0) {
    const auto current = model_data::Integer(from, key, minimum), target = model_data::Integer(to, key, minimum);
    return {{"id", id}, {"current", current ? Json(*current) : Json(nullptr)}, {"target", target ? Json(*target) : Json(nullptr)},
        {"complete", current.has_value() && target.has_value()}, {"steps", Json::array()}, {"provenance", Json::array()}};
}
inline void Missing(Json& track, std::string_view reason) { track["complete"] = false; track["reason_code"] = reason; }
inline bool Interval(Json& track, std::size_t available_rows) {
    if (!track.at("complete").get<bool>()) { Missing(track, "profile_unset"); return false; }
    const auto current = track.at("current").get<std::int32_t>(), target = track.at("target").get<std::int32_t>();
    if (target < current) throw std::runtime_error("A cultivation target cannot decrease progression");
    if (static_cast<std::size_t>(target - current) > available_rows) { Missing(track, "growth_interval_unavailable"); return false; }
    return true;
}
template<class Reader> void Provenance(Reader& reader, Json& track, std::string_view section, model_data::Row row) {
    track["provenance"].push_back({{"dataset", section}, {"row", reader.Name(row.id)}});
}
inline void Costs(Json& step, const std::optional<Stock>& materials, const std::optional<Stock>& currency) {
    Stock values;
    if (materials) for (const auto& [id, count] : *materials) if (!Add(values, id, count)) throw std::runtime_error("A native cultivation cost exceeds the supported quantity");
    if (currency) for (const auto& [id, count] : *currency) if (!Add(values, id, count)) throw std::runtime_error("A native cultivation cost exceeds the supported quantity");
    const auto gold = values.find("Gold");
    step["gold"] = materials && currency ? Json(gold == values.end() ? 0 : gold->second) : Json(nullptr);
    if (gold != values.end()) values.erase(gold);
    step["items"] = materials && currency ? Json(values) : Json(nullptr);
}
template<class Reader> Json ExperienceMaterials(Reader& reader, bool fork, bool& complete) {
    Json materials = Json::array(); complete = true;
    for (const auto id : reader.ExperienceCardIds(fork)) {
        const auto name = reader.Name(id);
        const auto definition = model_data::ById(reader, "items", id);
        if (!definition) { complete = false; continue; }
        const auto row = reader.CatalogRow("items", definition.id, definition.address);
        if (!row.value("element_complete", false) || !row.contains("material_experience") ||
            row.at("material_experience").template get<std::int64_t>() <= 0) { complete = false; continue; }
        const auto costs = ParseCosts(row.at("material_gold_cost_raw").template get_ref<const std::string&>());
        Json gold;
        if (costs) {
            bool only_gold = true;
            for (const auto& [item, quantity] : *costs) if (item != "Gold" && quantity) only_gold = false;
            if (!only_gold) complete = false; // The math contract has a per-book Gold fee, not arbitrary hidden fees.
            const auto found = costs->find("Gold"); gold = found == costs->end() ? Json(0) : Json(found->second);
        } else complete = false;
        materials.push_back({{"item_id", name}, {"experience", row.at("material_experience")}, {"gold_cost", gold},
            {"source", fork ? "CharacterDataAsset.ForkExpCards" : "CharacterDataAsset.CharacterExpCards"},
            {"definition_dataset", "items"}, {"source_row", name}});
    }
    if (materials.empty()) complete = false;
    return materials;
}
template<class Reader> std::optional<std::int32_t> RequiredStage(Reader& reader, std::string_view section,
                                                               std::uint64_t prefix, std::int32_t level) {
    const auto count = reader.CatalogTableFor(section).rows.size();
    for (std::size_t stage{}; stage < count; ++stage) {
        const auto row = model_data::Numbered(reader, section, prefix, static_cast<std::int32_t>(stage));
        if (!row) return std::nullopt;
        if (reader.template Field<std::int32_t>(row.address, "breakthroughMaxLevel") >= level) return static_cast<std::int32_t>(stage);
    }
    return std::nullopt;
}
template<class Reader> Json Levels(Reader& reader, const Json& from, const Json& to, std::uintptr_t element, bool fork) {
    const auto section = fork ? "fork_upgrade" : "character_upgrade";
    const auto stages = fork ? "fork_breakthrough" : "character_breakthrough";
    const auto track_id = fork ? "fork-level" : "character-level", stage_id = fork ? "fork-breakthrough" : "character-breakthrough";
    auto track = Track(track_id, from, to, fork ? "fork_level" : "character_level", 1);
    if (!Interval(track, reader.CatalogTableFor(section).rows.size())) return track;
    const auto current = track.at("current").template get<std::int32_t>(), target = track.at("target").template get<std::int32_t>();
    const auto prefix = reader.template Field<std::uint64_t>(element, fork ? "forkUpgradePack" : "characterUpgradePack");
    const auto stage_prefix = reader.template Field<std::uint64_t>(element, fork ? "forkBreakthroughPack" : "characterBreakthroughPack");
    if (!model_data::Numbered(reader, section, prefix, current) || !model_data::Numbered(reader, section, prefix, target)) {
        Missing(track, "growth_row_missing"); return track;
    }
    if (current == target) return track;
    track["row_semantics"] = fork ? "destination_level" : "from_level";
    for (auto level = current; level < target; ++level) {
        const auto row = model_data::Numbered(reader, section, prefix, fork ? level + 1 : level);
        const auto stage = RequiredStage(reader, stages, stage_prefix, level + 1);
        if (!row || !stage) { Missing(track, "growth_row_missing"); break; }
        const auto experience = reader.template Field<std::int32_t>(row.address, "growthNeedExperience");
        if (experience < 0) { Missing(track, "growth_experience_invalid"); break; }
        track["steps"].push_back({{"from", level}, {"to", level + 1}, {"experience", experience}, {"items", Json::object()},
            {"gold", 0}, {"requires", Json::array({{{"track", stage_id}, {"minimum", *stage}}})}});
        progression_data::Provenance(reader, track, section, row);
    }
    bool materials_complete{};
    track["experience_materials"] = ExperienceMaterials(reader, fork, materials_complete);
    if (!materials_complete) Missing(track, "experience_materials_incomplete");
    const auto experience = from.find(fork ? "fork_experience" : "character_experience");
    if (experience != from.end()) { track["already_experience"] = *experience; track["experience_scope"] = "observed_remaining_interval"; }
    else track["experience_scope"] = "full_level_interval";
    return track;
}
template<class Reader> Json Breakthroughs(Reader& reader, const Json& from, const Json& to, std::uintptr_t element, bool fork) {
    const auto section = fork ? "fork_breakthrough" : "character_breakthrough";
    auto track = Track(fork ? "fork-breakthrough" : "character-breakthrough", from, to,
        fork ? "fork_breakthrough_stage" : "character_breakthrough_stage");
    if (!Interval(track, reader.CatalogTableFor(section).rows.size())) return track;
    const auto prefix = reader.template Field<std::uint64_t>(element, fork ? "forkBreakthroughPack" : "characterBreakthroughPack");
    if (!model_data::Numbered(reader, section, prefix, track.at("current").template get<std::int32_t>()) ||
        !model_data::Numbered(reader, section, prefix, track.at("target").template get<std::int32_t>())) {
        Missing(track, "breakthrough_row_missing"); return track;
    }
    track["row_semantics"] = "destination_stage";
    for (auto stage = track.at("current").template get<std::int32_t>(); stage < track.at("target").template get<std::int32_t>(); ++stage) {
        const auto row = model_data::Numbered(reader, section, prefix, stage + 1), previous = model_data::Numbered(reader, section, prefix, stage);
        if (!row || !previous) { Missing(track, "breakthrough_row_missing"); break; }
        Json step{{"from", stage}, {"to", stage + 1}, {"requires", Json::array({{{"track", fork ? "fork-level" : "character-level"},
            {"minimum", reader.template Field<std::int32_t>(previous.address, "breakthroughMaxLevel")}}})}};
        Costs(step, ParseCosts(reader.GameString(row.address + reader.V("breakthroughNeedItems"))),
            ParseCosts(reader.GameString(row.address + reader.V("breakthroughNeedGolds"))));
        if (step.at("items").is_null()) Missing(track, "breakthrough_cost_string_unreadable");
        track["steps"].push_back(std::move(step)); progression_data::Provenance(reader, track, section, row);
    }
    return track;
}
template<class Reader> Json Awakening(Reader& reader, const Json& from, const Json& to, std::uintptr_t element, model_data::Row role) {
    auto track = Track("awakening", from, to, "awakening_level");
    const auto maximum = reader.template Field<std::int32_t>(element, "characterMaxAwaken");
    if (maximum < 0 || !Interval(track, static_cast<std::size_t>(maximum))) return track;
    const auto current = track.at("current").template get<std::int32_t>(), target = track.at("target").template get<std::int32_t>();
    if (current > maximum || target > maximum) { Missing(track, "awakening_level_outside_catalog"); return track; }
    if (current == target) return track;
    // The official awaken confirmation checks this fixed string once, then
    // submits one ServerCharacterAwaken(UID, EffectID); its quantity has no
    // level multiplier. This models costs only, not conditional effect buffs.
    const auto costs = ParseCosts(reader.GameString(element + reader.V("characterAwakenCost")));
    if (!costs) { Missing(track, "awakening_cost_string_unreadable"); return track; }
    for (auto level = current; level < target; ++level) {
        Json step{{"from", level}, {"to", level + 1}};
        Costs(step, costs, Stock{}); track["steps"].push_back(std::move(step));
    }
    track["cost_semantics"] = "fixed_per_awakening";
    track["cost_field"] = "CharacterItemData.AwakenCostItem";
    progression_data::Provenance(reader, track, "characters", role);
    return track;
}
inline std::pair<std::string, int> Tier(std::string_view id) {
    if (id.size() < 5 || id.substr(id.size() - 4, 3) != "_lv" || id.back() < '1' || id.back() > '3') return {};
    const auto family = id.substr(0, id.size() - 4); bool allowed = family == "EquipmentUpMaterial";
    for (const auto& [prefix, last] : {std::pair{"OrdinaryMonMaterial_0", '4'}, {"SkillUpMaterial_0", '5'}, {"WeaponBreakMaterial_0", '5'}}) {
        const std::string_view spelling(prefix);
        if (family.size() == spelling.size() + 1 && family.starts_with(spelling) && family.back() >= '1' && family.back() <= last) allowed = true;
    }
    return allowed ? std::pair<std::string, int>{family, id.back() - '0'} : std::pair<std::string, int>{};
}
template<class Reader> Json Recipes(Reader& reader) {
    Json result = Json::array();
    for (const auto& [id, address] : reader.CatalogTableFor("material_recipes").rows) {
        const auto row = reader.CatalogRow("material_recipes", id, address);
        const auto name = reader.Name(id);
        const auto higher = Tier(name); if (higher.first.empty() || higher.second <= 1) continue;
        const auto lower = higher.first + "_lv" + std::to_string(higher.second - 1);
        Stock costs; bool valid = row.value("recipe_complete", false) && !row.value("is_base", true);
        for (const auto& piece : row.at("pieces")) {
            if (!piece.at("id").is_string()) { valid = false; continue; }
            const auto amount = Count(piece.at("amount").template get_ref<const std::string&>());
            if (!amount || !*amount || !Add(costs, Canonical(piece.at("id").template get_ref<const std::string&>()), *amount)) valid = false;
        }
        if (!valid || !costs.contains(lower) || costs.at(lower) != 3) continue;
        const auto gold = costs.find("Gold"); const auto fee = gold == costs.end() ? 0 : gold->second;
        costs.erase(lower); costs.erase("Gold"); if (!costs.empty()) continue;
        result.push_back({{"from", lower}, {"to", name}, {"ratio", 3}, {"gold_cost", fee},
            {"source", "material_recipes"}, {"source_row", name}});
    }
    return result;
}
template<class Reader> void Skills(Reader& reader, model_data::Row role, const Json& from, const Json& to, Json& tracks) {
    const auto target = to.find("skill_levels"), current = from.find("skill_levels");
    if (target == to.end() || !target->is_object()) {
        auto track = Track("skills", Json::object(), Json::object(), "level"); Missing(track, "profile_unset"); tracks.push_back(std::move(track)); return;
    }
    const auto definition = model_data::ById(reader, "character_skills", role.id);
    const auto config = definition ? reader.CatalogRow("character_skills", definition.id, definition.address) : Json::object();
    for (const auto& [id, level] : target->items()) {
        const Json* skill{};
        if (config.contains("skills")) for (const auto& row : config.at("skills"))
            if (model_data::SameIdentifier(row.at("id").template get_ref<const std::string&>(), id)) skill = &row;
        Json origin = Json::object(), destination{{"level", level}};
        if (current != from.end() && current->is_object()) for (const auto& [name, value] : current->items())
            if (model_data::SameIdentifier(name, id)) origin["level"] = value;
        auto track = Track("skill-" + id, origin, destination, "level", 1);
        if (!skill || !skill->value("costs_complete", false)) Missing(track, "skill_identity_or_costs_missing");
        else if (Interval(track, skill->at("levels").size())) {
            if (skill->at("max_level").is_null() || track.at("current") > skill->at("max_level") || track.at("target") > skill->at("max_level")) {
                Missing(track, "skill_level_outside_catalog"); tracks.push_back(std::move(track)); continue;
            }
            track["row_semantics"] = "from_level"; progression_data::Provenance(reader, track, "character_skills", definition);
            for (auto value = track.at("current").template get<std::int32_t>(); value < track.at("target").template get<std::int32_t>(); ++value) {
                const Json* source{};
                for (const auto& row : skill->at("levels")) if (row.at("level") == value) { if (source) throw std::runtime_error("An official skill cost edge is duplicated"); source = &row; }
                if (!source) { Missing(track, "skill_cost_row_missing"); break; }
                Stock costs; bool valid = true;
                for (const auto& item : source->at("costs")) {
                    if (!item.at("id").is_string()) { valid = false; continue; }
                    const auto amount = Count(item.at("amount").template get_ref<const std::string&>());
                    if (!amount || !*amount || !Add(costs, Canonical(item.at("id").template get_ref<const std::string&>()), *amount)) valid = false;
                }
                Json step{{"from", value}, {"to", value + 1}, {"requires", Json::array({
                    {{"track", "character-breakthrough"}, {"minimum", source->at("breakthrough_stage")}},
                    {{"track", "awakening"}, {"minimum", source->at("awakening_level")}}})}};
                Costs(step, valid ? std::optional<Stock>(costs) : std::nullopt, Stock{});
                if (!valid) Missing(track, "skill_cost_invalid");
                track["steps"].push_back(std::move(step));
            }
        }
        tracks.push_back(std::move(track));
    }
}
template<class Reader> Json Build(Reader& reader, const Json& request) {
    if (!request.is_object() || !request.contains("plans") || !request.at("plans").is_array() || request.at("plans").empty() || request.at("plans").size() > 64)
        throw std::runtime_error("A bounded cultivation plan list is required");
    const bool include_owned = request.value("include_owned", true);
    Json plans = Json::array(); std::set<std::string> plan_ids, relevant{"Gold"}; bool complete = true;
    for (const auto& input : request.at("plans")) {
        const auto id = input.at("id").template get<std::string>(), role_id = input.at("role_id").template get<std::string>();
        if (id.empty() || id.size() > 128 || !plan_ids.insert(id).second) throw std::runtime_error("Cultivation plan identities must be unique");
        const auto role = model_data::ByName(reader, "characters", role_id);
        if (!role) throw std::runtime_error("The cultivation role has no official identity");
        const auto element = model_data::Element(reader, role, "CharacterItemData");
        Json from = input.value("from", Json::object()); const auto& to = input.at("to");
        if (!from.is_object() || !to.is_object()) throw std::runtime_error("Cultivation profiles must be objects");
        const bool observed = input.value("use_observed", request.value("use_observed", false));
        if (observed) from.update(reader.ObservedCultivationProfile(role_id));
        Json tracks = Json::array();
        tracks.push_back(Levels(reader, from, to, element, false)); tracks.push_back(Breakthroughs(reader, from, to, element, false));
        tracks.push_back(Awakening(reader, from, to, element, role)); Skills(reader, role, from, to, tracks);
        const auto target_fork = to.find("fork_id");
        const auto current_fork = from.find("fork_id");
        if (target_fork == to.end() || !target_fork->is_string()) {
            auto fork = Track("fork", Json::object(), Json::object(), "level"); Missing(fork, "profile_unset"); tracks.push_back(std::move(fork));
        } else if (!target_fork->template get_ref<const std::string&>().empty()) {
            const auto item = model_data::ByName(reader, "fork_templates", target_fork->template get_ref<const std::string&>());
            Json fork_from = from;
            if (current_fork == from.end() || !current_fork->is_string() ||
                !model_data::SameIdentifier(current_fork->template get_ref<const std::string&>(), target_fork->template get_ref<const std::string&>())) {
                for (const char* key : {"fork_level", "fork_breakthrough_stage", "fork_refinement", "fork_experience"}) fork_from[key] = nullptr;
            }
            if (!item) {
                auto fork = Track("fork", Json::object(), Json::object(), "level"); Missing(fork, "fork_identity_missing"); tracks.push_back(std::move(fork));
            } else {
                const auto data = model_data::Element(reader, item, "ForkItemData");
                tracks.push_back(Levels(reader, fork_from, to, data, true)); tracks.push_back(Breakthroughs(reader, fork_from, to, data, true));
                auto refinement = Track("fork-refinement", fork_from, to, "fork_refinement", 1);
                if (refinement.at("complete").template get<bool>()) {
                    const auto maximum = reader.template Field<std::int32_t>(data, "forkMaxRefinement");
                    if (refinement.at("current") > maximum || refinement.at("target") > maximum) Missing(refinement, "refinement_level_outside_catalog");
                    else if (refinement.at("current") != refinement.at("target")) Missing(refinement, "refinement_material_cost_unverified");
                }
                tracks.push_back(std::move(refinement));
            }
        }
        Json incomplete = Json::array(), gaps = Json::array();
        for (const auto& track : tracks) {
            if (!track.at("complete").template get<bool>()) { incomplete.push_back(track.at("id")); gaps.push_back({{"track", track.at("id")}, {"reason_code", track.value("reason_code", "profile_unset")}}); complete = false; }
            for (const auto& step : track.at("steps")) {
                if (step.contains("gold") && step.at("gold").is_null()) complete = false;
                if (step.contains("items") && step.at("items").is_object())
                    for (const auto& [item, quantity] : step.at("items").items()) { (void)quantity; relevant.insert(item); }
            }
            if (track.contains("experience_materials")) for (const auto& material : track.at("experience_materials")) {
                relevant.insert(material.at("item_id").template get<std::string>());
                if (material.at("gold_cost").is_null()) complete = false;
            }
        }
        plans.push_back({{"id", id}, {"role_id", reader.Name(role.id)}, {"tracks", std::move(tracks)}, {"incomplete_tracks", std::move(incomplete)},
            {"gaps", std::move(gaps)}, {"from_source", observed ? "live_inventory" : "explicit_profile"}});
    }
    auto recipes = Recipes(reader);
    for (const auto& recipe : recipes) { relevant.insert(recipe.at("from").template get<std::string>()); relevant.insert(recipe.at("to").template get<std::string>()); }
    Json owned = Json::object(), owned_gaps = Json::array(); std::string owned_diagnostic;
    if (include_owned) for (const auto& id : relevant) {
        try { owned[id] = reader.OwnedAmount(id); }
        catch (const std::exception& exception) { owned_gaps.push_back(id); if (owned_diagnostic.empty()) owned_diagnostic = exception.what(); }
    }
    const bool owned_complete = include_owned && owned_gaps.empty();
    return {{"phase", "ready"}, {"complete", complete && owned_complete}, {"plans", std::move(plans)},
        {"owned", owned_complete ? owned : Json(nullptr)}, {"known_owned", std::move(owned)}, {"owned_complete", owned_complete},
        {"owned_gaps", std::move(owned_gaps)}, {"owned_source", include_owned ? "HTInventoryComponent.GetItemTotalAmount" : "unavailable_account_snapshot"},
        {"owned_includes_bound", include_owned ? Json(true) : Json(nullptr)},
        {"owned_diagnostic", owned_diagnostic.empty() ? Json(nullptr) : Json(owned_diagnostic)},
        {"owned_scope", include_owned ? "all_relevant_official_materials" : "unavailable_account_snapshot"}, {"allow_crafting", request.value("allow_crafting", true)},
        {"recipes", std::move(recipes)}, {"source_kind", include_owned ? "live_data_table_and_inventory" : "live_data_table"}};
}

}  // namespace equipment_calculator::progression_data
