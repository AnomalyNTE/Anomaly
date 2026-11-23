#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace equipment_calculator::catalog_data {

// Unreflected container compatibility remains in Profile. Fields below are
// bound by their real UScriptStruct property names, kinds, widths and bounds;
// no second copy of a game field offset is stored in source code.
template<class Reader>
std::uint32_t Field(Reader& reader, std::uintptr_t owner, std::string_view name,
                    std::string_view kind, std::size_t width, std::string_view value_type = {},
                    std::uintptr_t known_property = 0) {
    const auto property = known_property ? known_property : reader.FindProperty(owner, name);
    const auto field_class = reader.template Field<std::uintptr_t>(property, "fieldClass");
    const auto actual_kind = reader.Name(reader.template Field<std::uint32_t>(field_class, "fieldClassName"));
    const auto offset = reader.template Field<std::int32_t>(property, "propertyOffset");
    const auto size = reader.template Field<std::int32_t>(owner, "structSize");
    if ((actual_kind != kind && !(kind == "EnumProperty" && actual_kind == "ByteProperty")) ||
        reader.template Field<std::int32_t>(property, "propertyArrayDim") != 1 ||
        reader.template Field<std::int32_t>(property, "propertyElementSize") != static_cast<std::int32_t>(width) ||
        offset < 0 || size < 0 || static_cast<std::size_t>(offset) > static_cast<std::size_t>(size) ||
        width > static_cast<std::size_t>(size) - static_cast<std::size_t>(offset))
        throw std::runtime_error("The live catalog property contract is unsupported: " + std::string(name));
    if (!value_type.empty()) {
        const auto inner = kind == "ArrayProperty" ? reader.template Field<std::uintptr_t>(property, "arrayInner") : property;
        const auto type = reader.template Field<std::uintptr_t>(inner, "propertyStruct");
        if (!type || reader.ObjectName(type) != value_type)
            throw std::runtime_error("The live catalog value schema is unsupported: " + std::string(name));
    }
    return static_cast<std::uint32_t>(offset);
}

template<class Reader>
std::uint32_t Size(Reader& reader, std::uintptr_t owner) {
    const auto size = reader.template Field<std::int32_t>(owner, "structSize");
    if (size <= 0 || size > 65536) throw std::runtime_error("The live catalog record size is unsupported");
    return static_cast<std::uint32_t>(size);
}

struct ImagePaths {
    std::uint32_t soft_path{}, soft_path_size{}, soft_reference_size{}, asset_path{}, package{}, asset{}, sub_path{};
    std::array<std::uint32_t, 3> item_icons{};
    std::uint32_t plan_icon{}, appearance_table{}, appearance_size{}, appearance_type{}, appearance_character{}, appearance_default{}, appearance_exclusions{};
    std::array<std::uint32_t, 4> appearance_icons{};
    std::uint32_t character_appearances{};
};

template<class Reader> ImagePaths DescribeImagePaths(Reader& reader) {
    using Array = typename Reader::NativeArray;
    ImagePaths result;
    const auto path = reader.Exact("/Script/CoreUObject.SoftObjectPath").second;
    const auto asset = reader.Exact("/Script/CoreUObject.TopLevelAssetPath").second;
    result.soft_path = reader.V("softObjectPathOffset");
    result.soft_path_size = Size(reader, path);
    if (result.soft_path > 1024 || result.soft_path_size > 1024 - result.soft_path)
        throw std::runtime_error("The soft object reference layout is unsupported");
    result.soft_reference_size = result.soft_path + result.soft_path_size;
    result.asset_path = Field(reader, path, "AssetPath", "StructProperty", Size(reader, asset), "TopLevelAssetPath");
    result.package = Field(reader, asset, "PackageName", "NameProperty", sizeof(std::uint64_t));
    result.asset = Field(reader, asset, "AssetName", "NameProperty", sizeof(std::uint64_t));
    result.sub_path = Field(reader, path, "SubPathString", "Utf8StrProperty", sizeof(Array));
    const auto item = reader.Class("StaticItemData");
    constexpr std::array item_fields{"ItemIcon", "ItemIconSmall", "ItemIconBig"};
    for (std::size_t index{}; index < item_fields.size(); ++index)
        result.item_icons[index] = Field(reader, item, item_fields[index], "SoftObjectProperty", result.soft_reference_size, "Texture2D");
    result.plan_icon = Field(reader, reader.Class("EquipmentItemData"), "PlanIcon", "SoftObjectProperty", result.soft_reference_size, "Texture2D");
    result.appearance_table = Field(reader, reader.Class("CharacterDataAsset"), "AppearanceDataTable", "ObjectProperty", sizeof(std::uintptr_t));
    const auto appearance = reader.Class("StaticAppearanceData");
    result.appearance_size = Size(reader, appearance);
    result.appearance_type = Field(reader, appearance, "AppearanceType", "EnumProperty", sizeof(std::uint8_t));
    result.appearance_character = Field(reader, appearance, "CharacterID", "NameProperty", sizeof(std::uint64_t));
    result.appearance_default = Field(reader, appearance, "IsDefault", "BoolProperty", sizeof(std::uint8_t));
    constexpr std::array appearance_fields{"HeadIcon", "HeadIconBig", "PortraitImg", "SmallPortraitImg"};
    for (std::size_t index{}; index < appearance_fields.size(); ++index)
        result.appearance_icons[index] = Field(reader, appearance, appearance_fields[index], "SoftObjectProperty", result.soft_reference_size, "Texture2D");
    const auto name_array = [&](std::uintptr_t owner, std::string_view name) {
        const auto property = reader.FindProperty(owner, name);
        const auto offset = Field(reader, owner, name, "ArrayProperty", sizeof(Array), {}, property);
        const auto inner = reader.template Field<std::uintptr_t>(property, "arrayInner");
        const auto inner_class = reader.template Field<std::uintptr_t>(inner, "fieldClass");
        if (reader.Name(reader.template Field<std::uint32_t>(inner_class, "fieldClassName")) != "NameProperty" ||
            reader.template Field<std::int32_t>(inner, "propertyArrayDim") != 1 ||
            reader.template Field<std::int32_t>(inner, "propertyElementSize") != static_cast<std::int32_t>(sizeof(std::uint64_t)))
            throw std::runtime_error("The appearance identity array schema is unsupported");
        return offset;
    };
    result.character_appearances = name_array(reader.Class("HTCharacterItem"), "AppearanceEquipData");
    result.appearance_exclusions = name_array(appearance, "ExcludeCharacterID");
    return result;
}

template<class Reader> nlohmann::json ReadImagePath(Reader& reader, const ImagePaths& layout, std::uintptr_t reference) {
    using Array = typename Reader::NativeArray;
    const auto bytes = reader.Block(reference + layout.soft_path, layout.soft_path_size);
    const auto package = Reader::template ExtractAt<std::uint64_t>(bytes, layout.asset_path + layout.package);
    const auto asset = Reader::template ExtractAt<std::uint64_t>(bytes, layout.asset_path + layout.asset);
    const auto sub_path = reader.GameUtf8String(Reader::template ExtractAt<Array>(bytes, layout.sub_path));
    if (!package && !asset && sub_path.empty()) return nullptr;
    if (!package || !asset) throw std::runtime_error("The image soft reference has an incomplete asset identity");
    auto path = reader.Name(package);
    if (path.empty() || path.front() != '/') throw std::runtime_error("The image soft reference package is unsupported");
    path += "." + reader.Name(asset);
    if (!sub_path.empty()) path += ":" + sub_path;
    return path;
}

struct FloatPack {
    std::uint32_t size{};
    std::vector<std::pair<std::uint64_t, std::uint32_t>> properties;
};
template<class Reader>
FloatPack DescribeFloatPack(Reader& reader) {
    const auto type = reader.Class("PackIDStaticData");
    FloatPack result; result.size = Size(reader, type);
    auto property = reader.template Field<std::uintptr_t>(type, "structProperties");
    for (unsigned guard{}; property && guard < 1024; ++guard) {
        const auto id = reader.template Field<std::uint64_t>(property, "fieldName");
        const auto name = reader.Name(id);
        result.properties.emplace_back(id, Field(reader, type, name, "FloatProperty", sizeof(float), {}, property));
        property = reader.template Field<std::uintptr_t>(property, "fieldNext");
    }
    if (property || result.properties.empty()) throw std::runtime_error("The base attribute pack property list is incomplete");
    return result;
}
template<class Reader>
nlohmann::json ReadFloatPack(Reader& reader, const FloatPack& layout, std::uintptr_t address) {
    const auto data = reader.Block(address, layout.size);
    nlohmann::json attributes = nlohmann::json::array();
    bool units_complete = true;
    for (const auto& [id, offset] : layout.properties) {
        const auto value = Reader::template ExtractAt<float>(data, offset);
        if (!std::isfinite(value)) throw std::runtime_error("A base attribute pack value is not finite");
        const auto* definition = reader.Attribute(id);
        if (!definition) units_complete = false;
        const auto name = reader.Name(id);
        nlohmann::json attribute{{"id", name}, {"name", definition ? definition->name : name},
            {"value", value}, {"raw_value", value}, {"source", "base_pack"}, {"percent", nullptr}};
        if (definition) attribute["percent"] = definition->percent;
        attributes.push_back(std::move(attribute));
    }
    return {{"attributes", std::move(attributes)}, {"values_complete", true}, {"units_complete", units_complete},
        {"attributes_scope", "static_base_pack"}, {"attributes_complete", true}};
}

struct Modifiers {
    std::uint32_t conditions{}, values{}, stride{}, id{}, value{}, operation{}, sort{};
};
template<class Reader>
Modifiers DescribeModifiers(Reader& reader) {
    const auto row = reader.Class("ModifyStaticData"), value = reader.Class("ModifyDataStruct");
    using Array = typename Reader::NativeArray;
    return {Field(reader, row, "ConditionArray", "ArrayProperty", sizeof(Array), "InstancedStruct"),
        Field(reader, row, "ModifyData", "ArrayProperty", sizeof(Array), "ModifyDataStruct"), Size(reader, value),
        Field(reader, value, "PropName", "NameProperty", sizeof(std::uint64_t)),
        Field(reader, value, "PropValue", "FloatProperty", sizeof(float)),
        Field(reader, value, "ModifierOp", "EnumProperty", sizeof(std::uint8_t)),
        Field(reader, value, "SortKey", "IntProperty", sizeof(std::int32_t))};
}
template<class Reader>
nlohmann::json ReadModifiers(Reader& reader, const Modifiers& layout, std::uintptr_t address) {
    using Array = typename Reader::NativeArray;
    const auto conditions = reader.CheckedArray(reader.template ReadValue<Array>(address + layout.conditions), 1024);
    std::int32_t meaningful_conditions{};
    reader.VisitArray(conditions, reader.V("instancedStructSize"), 1024, [&](auto data) {
        if (reader.template Extract<std::uintptr_t>(data, "instanceStructType") ||
            reader.template Extract<std::uintptr_t>(data, "instanceStructMemory")) ++meaningful_conditions;
    });
    nlohmann::json values = nlohmann::json::array(); bool operations_complete = true;
    reader.VisitArray(reader.template ReadValue<Array>(address + layout.values), layout.stride, 1024, [&](auto data) {
        const auto id = Reader::template ExtractAt<std::uint64_t>(data, layout.id);
        const auto value = Reader::template ExtractAt<float>(data, layout.value);
        const auto operation_id = Reader::template ExtractAt<std::uint8_t>(data, layout.operation);
        if (!id || !std::isfinite(value)) throw std::runtime_error("A static attribute modifier is invalid");
        const auto operation = reader.ModifierOperation(operation_id);
        if (operation.empty()) operations_complete = false;
        values.push_back({{"id", reader.Name(id)}, {"value", value}, {"operation_id", operation_id},
            {"operation", operation.empty() ? nlohmann::json(nullptr) : nlohmann::json(operation)},
            {"sort", Reader::template ExtractAt<std::int32_t>(data, layout.sort)}});
    });
    return {{"modifiers", std::move(values)}, {"conditions_count", meaningful_conditions}, {"condition_records", conditions.count},
        {"unconditional", meaningful_conditions == 0}, {"conditions_complete", meaningful_conditions == 0},
        {"values_complete", true}, {"operations_complete", operations_complete},
        {"permanent_attributes_complete", meaningful_conditions == 0 && operations_complete}};
}

struct Recipes {
    std::uint32_t pieces{}, base{}, group{}, is_base{}, rate{}, stride{}, piece{}, amount{};
};
template<class Reader>
Recipes DescribeRecipes(Reader& reader) {
    const auto row = reader.Class("ComposeItemData"), value = reader.Class("PieceItemInfo");
    using Array = typename Reader::NativeArray;
    return {Field(reader, row, "PieceItems", "ArrayProperty", sizeof(Array), "PieceItemInfo"),
        Field(reader, row, "BaseItemID", "NameProperty", sizeof(std::uint64_t)),
        Field(reader, row, "GroupIndex", "IntProperty", sizeof(std::int32_t)),
        Field(reader, row, "bIsBaseItem", "BoolProperty", sizeof(std::uint8_t)),
        Field(reader, row, "ExchangeRate", "IntProperty", sizeof(std::int32_t)), Size(reader, value),
        Field(reader, value, "PieceItemID", "NameProperty", sizeof(std::uint64_t)),
        Field(reader, value, "PieceAmount", "Int64Property", sizeof(std::int64_t))};
}
template<class Reader>
nlohmann::json ReadRecipe(Reader& reader, const Recipes& layout, std::uintptr_t address) {
    using Array = typename Reader::NativeArray;
    nlohmann::json pieces = nlohmann::json::array();
    reader.VisitArray(reader.template ReadValue<Array>(address + layout.pieces), layout.stride, 64, [&](auto value) {
        pieces.push_back({{"id", reader.OptionalName(Reader::template ExtractAt<std::uint64_t>(value, layout.piece))},
            {"amount", std::to_string(Reader::template ExtractAt<std::int64_t>(value, layout.amount))}});
    });
    return {{"base_item_id", reader.OptionalName(reader.template ReadValue<std::uint64_t>(address + layout.base))},
        {"group", reader.template ReadValue<std::int32_t>(address + layout.group)},
        {"is_base", reader.template ReadValue<std::uint8_t>(address + layout.is_base) != 0},
        {"exchange_rate", reader.template ReadValue<std::int32_t>(address + layout.rate)},
        {"pieces", std::move(pieces)}, {"recipe_complete", true}};
}

struct SkillCosts {
    std::uint32_t abilities{}, info_size{}, type{}, index{}, levels{}, level_stride{}, level{}, breakthrough{}, awakening{},
        costs{}, cost_stride{}, cost_id{}, cost_amount{};
};
template<class Reader>
SkillCosts DescribeSkillCosts(Reader& reader) {
    const auto config = reader.Class("CharacterAbilityConfig"), info = reader.Class("CharacterAbilityInfo");
    const auto level = reader.Class("AbilityCostItem"), cost = reader.Class("HTCommonIdNumber");
    using Array = typename Reader::NativeArray;
    reader.StructSize(config, "abilityConfigSize"); reader.StructSize(info, "abilityInfoSize");
    reader.StructSize(level, "abilityCostSize"); reader.StructSize(cost, "commonIdNumberSize");
    return {Field(reader, config, "CharacterAbilityList", "MapProperty", reader.V("mapSize")), Size(reader, info),
        Field(reader, info, "AbilityType", "EnumProperty", sizeof(std::uint8_t)),
        Field(reader, info, "AbilityIndex", "IntProperty", sizeof(std::int32_t)),
        Field(reader, info, "LevelsCostItems", "ArrayProperty", sizeof(Array), "AbilityCostItem"), Size(reader, level),
        Field(reader, level, "Level", "IntProperty", sizeof(std::int32_t)),
        Field(reader, level, "RequireTupoLevel", "IntProperty", sizeof(std::int32_t)),
        Field(reader, level, "RequireAwakenLevel", "IntProperty", sizeof(std::int32_t)),
        Field(reader, level, "CostItems", "ArrayProperty", sizeof(Array), "HTCommonIdNumber"), Size(reader, cost),
        Field(reader, cost, "ID", "NameProperty", sizeof(std::uint64_t)),
        Field(reader, cost, "Number", "IntProperty", sizeof(std::int32_t))};
}
template<class Reader>
nlohmann::json ReadSkillCosts(Reader& reader, const SkillCosts& layout, std::uintptr_t address) {
    using Array = typename Reader::NativeArray;
    nlohmann::json skills = nlohmann::json::array();
    reader.VisitSparse(address + layout.abilities, reader.V("abilityMapStride"), [&](auto entry) {
        const auto id = Reader::template ExtractAt<std::uint64_t>(entry, 0);
        if (!id) throw std::runtime_error("An official character ability has no identity");
        const auto info = entry.subspan(reader.V("tableValue"), layout.info_size);
        nlohmann::json levels = nlohmann::json::array();
        std::int32_t maximum{};
        reader.VisitArray(Reader::template ExtractAt<Array>(info, layout.levels), layout.level_stride, 64, [&](auto value) {
            const auto level = Reader::template ExtractAt<std::int32_t>(value, layout.level);
            maximum = (std::max)(maximum, level);
            nlohmann::json costs = nlohmann::json::array();
            reader.VisitArray(Reader::template ExtractAt<Array>(value, layout.costs), layout.cost_stride, 64, [&](auto item) {
                costs.push_back({{"id", reader.OptionalName(Reader::template ExtractAt<std::uint64_t>(item, layout.cost_id))},
                    {"amount", std::to_string(Reader::template ExtractAt<std::int32_t>(item, layout.cost_amount))}});
            });
            levels.push_back({{"level", level}, {"breakthrough_stage", Reader::template ExtractAt<std::int32_t>(value, layout.breakthrough)},
                {"awakening_level", Reader::template ExtractAt<std::int32_t>(value, layout.awakening)}, {"costs", std::move(costs)}});
        });
        nlohmann::json skill{{"id", reader.Name(id)}, {"type", Reader::template ExtractAt<std::uint8_t>(info, layout.type)},
            {"index", Reader::template ExtractAt<std::int32_t>(info, layout.index)}, {"levels", std::move(levels)},
            {"costs_level_semantics", "from_level"}, {"costs_complete", true}, {"max_level", nullptr}};
        if (maximum > 0) skill["max_level"] = static_cast<std::int64_t>(maximum) + 1;
        skills.push_back(std::move(skill));
    });
    return {{"skills", std::move(skills)}, {"skills_complete", true}};
}

}  // namespace equipment_calculator::catalog_data
