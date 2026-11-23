#pragma once

#include "catalog_data.hpp"
#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace equipment_calculator::model_data {
using Json = nlohmann::json;

struct Layout {
    std::uint32_t slot_area{}, slot_modify{}, slot_description{}, likeability_modifiers{}, likeability_name{}, refinement_buffs{};
};
template<class Reader> Layout Describe(Reader& reader) {
    const auto slots = reader.Class("CharacterEquipmentSlotsData"), likeability = reader.Class("LikeabilityRoleData");
    using Array = typename Reader::NativeArray;
    return {catalog_data::Field(reader, slots, "OwnerGridCount", "IntProperty", sizeof(std::int32_t)),
        catalog_data::Field(reader, slots, "ModifyPropID", "NameProperty", sizeof(std::uint64_t)),
        catalog_data::Field(reader, slots, "Desc", "TextProperty", 16),
        catalog_data::Field(reader, likeability, "MapModifyData", "MapProperty", reader.V("mapSize")),
        catalog_data::Field(reader, likeability, "RoleNameText", "TextProperty", 16),
        catalog_data::Field(reader, reader.Class("StaticForkUpgradeStarData"), "Buffs", "ArrayProperty", sizeof(Array), "ForkStarBuff")};
}

inline bool SameIdentifier(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    const auto fold = [](unsigned char value) { return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value; };
    for (std::size_t index{}; index < left.size(); ++index)
        if (fold(static_cast<unsigned char>(left[index])) != fold(static_cast<unsigned char>(right[index]))) return false;
    return true;
}
inline std::string IdentifierKey(std::string_view value) {
    std::string result(value);
    for (auto& character : result) if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    return result;
}
struct Row { std::uint64_t id{}; std::uintptr_t address{}; explicit operator bool() const { return address != 0; } };
template<class Reader> Row ById(Reader& reader, std::string_view section, std::uint64_t id) {
    if (!id) return {};
    const auto& table = reader.CatalogTableFor(section);
    for (const auto& [key, address] : table.rows) if (key == id) return {key, address};
    return {};
}
template<class Reader> Row ByName(Reader& reader, std::string_view section, std::string_view name) {
    return reader.NamedCatalogRow(section, name);
}
template<class Reader> Row Numbered(Reader& reader, std::string_view section, std::uint64_t prefix, std::int32_t number) {
    if (!prefix || number < 0) return {};
    const auto suffix = static_cast<std::uint32_t>(number) + 1U;
    const auto spelling = reader.Name(prefix);
    Row result;
    for (const auto& [key, address] : reader.CatalogTableFor(section).rows) {
        if (static_cast<std::uint32_t>(key >> 32U) != suffix ||
            !SameIdentifier(reader.Name(static_cast<std::uint32_t>(key)), spelling)) continue;
        if (result) throw std::runtime_error("An official numbered growth identity is ambiguous");
        result = {key, address};
    }
    return result;
}
template<class Reader> std::uintptr_t Element(Reader& reader, Row row, std::string_view type_name) {
    const auto element = row.address + reader.V("staticItemElement");
    const auto type = reader.template Field<std::uintptr_t>(element, "instanceStructType");
    const auto data = reader.template Field<std::uintptr_t>(element, "instanceStructMemory");
    if (type != reader.Class(type_name) || !data) throw std::runtime_error("The selected role model element schema is unsupported");
    reader.CatalogElementLayout(type, type_name);
    return data;
}
inline std::optional<std::int32_t> Integer(const Json& object, const char* key, std::int32_t minimum = 0) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_integer()) return std::nullopt;
    const auto value = found->get<std::int64_t>();
    if (value < minimum || value > INT32_MAX) return std::nullopt;
    return static_cast<std::int32_t>(value);
}
inline Json Source(std::string_view id, bool complete = true) {
    return {{"id", id}, {"complete", complete}, {"unconditional", true}, {"attributes", Json::object()}, {"provenance", Json::array()}};
}
inline void Missing(Json& source, std::string_view reason) {
    source["complete"] = false;
    source["reason_code"] = reason;
}
template<class Reader> void Provenance(Reader& reader, Json& source, std::string_view dataset, Row row) {
    source["provenance"].push_back({{"dataset", dataset}, {"row", reader.Name(row.id)}});
}
template<class Reader> void AddModifiers(Reader& reader, Json& source, std::string_view section, Row row) {
    if (!row) { Missing(source, "modifier_row_missing"); return; }
    const auto data = reader.CatalogRow(section, row.id, row.address);
    Provenance(reader, source, section, row);
    if (!data.value("permanent_attributes_complete", false)) { Missing(source, "conditional_or_unknown_modifier"); return; }
    if (!source.contains("modifiers")) source["modifiers"] = Json::array();
    for (const auto& modifier : data.at("modifiers")) {
        // The role growth contract is additive. Cross-pack overwrite order has
        // not been established by this adapter, so it cannot assert a panel.
        if (modifier.at("operation") != "additive") Missing(source, "modifier_application_order_unknown");
        source["modifiers"].push_back(modifier);
    }
}
template<class Reader> bool StageValid(Reader& reader, std::string_view section, std::uint64_t prefix,
                                     std::int32_t level, std::int32_t stage, Row& row) {
    row = Numbered(reader, section, prefix, stage);
    if (!row || level > reader.template Field<std::int32_t>(row.address, "breakthroughMaxLevel")) return false;
    if (!stage) return true;
    const auto previous = Numbered(reader, section, prefix, stage - 1);
    return previous && level >= reader.template Field<std::int32_t>(previous.address, "breakthroughMaxLevel");
}
template<class Reader> Json Build(Reader& reader, const Layout& layout, std::string_view role_id, const Json& profile) {
    if (!profile.is_object()) throw std::runtime_error("The role profile is not an object");
    const auto role = ByName(reader, "characters", role_id);
    if (!role) throw std::runtime_error("The selected official character identity is missing");
    const auto role_data = Element(reader, role, "CharacterItemData");
    const auto definition = reader.CatalogRow("characters", role.id, role.address);
    Json character = Source("character");
    const auto base = ById(reader, "player_pack", reader.template Field<std::uint64_t>(role_data, "characterBaseModify"));
    if (!base) Missing(character, "base_pack_missing");
    else {
        const auto data = reader.CatalogRow("player_pack", base.id, base.address);
        if (!data.value("values_complete", false)) Missing(character, "base_pack_incomplete");
        for (const auto& attribute : data.at("attributes")) character["attributes"][attribute.at("id").template get<std::string>()] = attribute.at("value");
        Provenance(reader, character, "player_pack", base);
    }
    const auto level = Integer(profile, "character_level", 1), stage = Integer(profile, "character_breakthrough_stage");
    if (!level || !stage) Missing(character, "profile_unset");
    else {
        const auto growth = Numbered(reader, "player_modify", reader.template Field<std::uint64_t>(role_data, "characterUpgradeModifyPack"), *level);
        AddModifiers(reader, character, "player_modify", growth);
        Row breakthrough;
        if (!StageValid(reader, "character_breakthrough", reader.template Field<std::uint64_t>(role_data, "characterBreakthroughPack"), *level, *stage, breakthrough))
            Missing(character, "invalid_progression");
        else {
            Provenance(reader, character, "character_breakthrough", breakthrough);
            if (*stage) AddModifiers(reader, character, "player_modify", ById(reader, "player_modify", reader.template Field<std::uint64_t>(breakthrough.address, "breakthroughModify")));
        }
    }
    Json fork = Source("fork");
    const auto selected_fork = profile.find("fork_id");
    if (selected_fork == profile.end() || !selected_fork->is_string()) Missing(fork, "profile_unset");
    else if (!selected_fork->template get_ref<const std::string&>().empty()) {
        const auto item = ByName(reader, "fork_templates", selected_fork->template get_ref<const std::string&>());
        const auto fork_level = Integer(profile, "fork_level", 1), fork_stage = Integer(profile, "fork_breakthrough_stage"), refinement = Integer(profile, "fork_refinement", 1);
        if (!item || !fork_level || !fork_stage || !refinement) Missing(fork, "profile_unset");
        else {
            const auto fork_data = Element(reader, item, "ForkItemData");
            const auto growth = Numbered(reader, "fork_upgrade", reader.template Field<std::uint64_t>(fork_data, "forkUpgradePack"), *fork_level);
            if (!growth) Missing(fork, "growth_row_missing");
            else {
                Provenance(reader, fork, "fork_upgrade", growth);
                AddModifiers(reader, fork, "fork_modify", ById(reader, "fork_modify", reader.template Field<std::uint64_t>(growth.address, "forkUpgradeModify")));
            }
            Row breakthrough;
            if (!StageValid(reader, "fork_breakthrough", reader.template Field<std::uint64_t>(fork_data, "forkBreakthroughPack"), *fork_level, *fork_stage, breakthrough))
                Missing(fork, "invalid_progression");
            else {
                Provenance(reader, fork, "fork_breakthrough", breakthrough);
                if (*fork_stage) AddModifiers(reader, fork, "fork_modify", ById(reader, "fork_modify", reader.template Field<std::uint64_t>(breakthrough.address, "breakthroughModify")));
            }
            const auto star = Numbered(reader, "fork_refinement", reader.template Field<std::uint64_t>(fork_data, "forkRefinementPack"), *refinement);
            if (!star || *refinement > reader.template Field<std::int32_t>(fork_data, "forkMaxRefinement")) Missing(fork, "invalid_refinement");
            else {
                Provenance(reader, fork, "fork_refinement", star);
                using Array = typename Reader::NativeArray;
                const auto buffs = reader.CheckedArray(reader.template ReadValue<Array>(star.address + layout.refinement_buffs), 64);
                const auto parameters = reader.CheckedArray(reader.template Field<Array>(star.address, "forkRefinementData"), 64);
                if (buffs.count || parameters.count) Missing(fork, "refinement_effect_evidence_incomplete");
            }
        }
    }
    Json likeability = Source("likeability");
    const auto likeability_enabled = profile.find("likeability_10");
    if (likeability_enabled == profile.end() || !likeability_enabled->is_boolean()) Missing(likeability, "profile_unset");
    else if (likeability_enabled->template get<bool>()) {
        const auto id = reader.template Field<std::uint64_t>(role_data, "characterLikeability");
        if (id) {
            const auto entry = ById(reader, "likeability_roles", id);
            if (!entry) Missing(likeability, "likeability_identity_missing");
            else {
                Provenance(reader, likeability, "likeability_roles", entry);
                std::uint64_t modifier{};
                reader.VisitSparse(entry.address + layout.likeability_modifiers, reader.V("tableStride"), [&](auto value) {
                    if (Reader::template ExtractAt<std::int32_t>(value, 0) == 10)
                        modifier = reader.template Extract<std::uint64_t>(value, "tableValue");
                });
                if (modifier) AddModifiers(reader, likeability, "likeability_modify", ById(reader, "likeability_modify", modifier));
            }
        }
    }
    Json awakening = Source("awakening");
    const auto awakening_level = Integer(profile, "awakening_level");
    const auto enabled = profile.find("enabled_awakening_ids");
    if (!awakening_level || enabled == profile.end() || !enabled->is_array()) Missing(awakening, "profile_unset");
    else if (*awakening_level != 0 || !enabled->empty()) Missing(awakening, "awakening_effect_evidence_incomplete");
    Json furniture = Source("furniture");
    const auto selected_furniture = profile.find("furniture");
    if (selected_furniture == profile.end() || !selected_furniture->is_object()) Missing(furniture, "profile_unset");
    else for (const auto* id : {"AtkAdd", "CritDamageBase"}) {
        const auto value = selected_furniture->find(id);
        if (value == selected_furniture->end() || !value->is_number() || !std::isfinite(value->template get<double>())) Missing(furniture, "profile_unset");
        else furniture["attributes"][id] = *value;
    }
    Json slots = Source("slot_rule"); Json extra_shape;
    const auto slot_id = reader.template Field<std::uint64_t>(role_data, "characterEquipmentSlot");
    if (slot_id) {
        const auto slot = ById(reader, "character_slots", slot_id);
        if (!slot) Missing(slots, "slot_rule_missing");
        else {
            Provenance(reader, slots, "character_slots", slot);
            const auto area = reader.template ReadValue<std::int32_t>(slot.address + layout.slot_area);
            const auto effect = ById(reader, "equipment_slot_modify", reader.template ReadValue<std::uint64_t>(slot.address + layout.slot_modify));
            Json attributes = Source("slot_attributes"); AddModifiers(reader, attributes, "equipment_slot_modify", effect);
            if (area <= 0 || !attributes.at("complete").template get<bool>()) Missing(slots, "slot_rule_incomplete");
            else {
                Json buffs = Json::object();
                for (const auto& modifier : attributes.at("modifiers")) {
                    const auto id = modifier.at("id").template get<std::string>();
                    buffs[id] = buffs.value(id, 0.0) + modifier.at("value").template get<double>();
                }
                extra_shape = {{"area", area}, {"buffs", std::move(buffs)}};
            }
        }
    }
    const auto slots_complete = slots.at("complete").template get<bool>();
    Json sources = Json::array({std::move(character), std::move(fork), std::move(likeability), std::move(awakening), std::move(furniture), std::move(slots)});
    bool complete = true; Json required = Json::array();
    for (const auto& source : sources) { complete = complete && source.at("complete").template get<bool>(); required.push_back(source.at("id")); }
    const auto element = reader.EnumName("ECharacterElementType", reader.template Field<std::uint8_t>(role_data, "characterElement"));
    Json damage_property;
    for (const auto& [kind, property] : {std::pair{"COSMOS", "DamageUpCosmosBase"}, {"NATURE", "DamageUpNatureBase"},
            {"INCANTATION", "DamageUpIncantationBase"}, {"CHAOS", "DamageUpChaosBase"}, {"PSYCHE", "DamageUpPsycheBase"},
            {"LAKSHANA", "DamageUpLakshanaBase"}, {"PSYCHICALLY", "DamageUpPsychicallyBase"}})
        if (element == std::string("CHARACTER_ELEMENT_TYPE_") + kind) damage_property = property;
    Json result{{"phase", "ready"}, {"complete", complete}, {"character_id", reader.Name(role.id)}, {"name", definition.at("name")},
        {"character_level", level ? Json(*level) : Json(nullptr)}, {"sources", std::move(sources)}, {"required_sources", std::move(required)},
        {"element", element}, {"damage_property", damage_property}, {"damage_property_complete", !damage_property.is_null()},
        {"extra_shape_complete", slots_complete}, {"source_kind", "live_data_table"}};
    if (!extra_shape.is_null()) result["extra_shape"] = std::move(extra_shape);
    return result;
}

}  // namespace equipment_calculator::model_data
