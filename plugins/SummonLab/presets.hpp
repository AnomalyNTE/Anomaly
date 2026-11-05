#pragma once

#include "attribute_catalog.hpp"
#include "blueprint_edits.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace summon_lab {

inline constexpr std::uint32_t kMaximumLevel = 2147483647;
inline constexpr std::size_t kNoPreset = std::numeric_limits<std::size_t>::max();
inline constexpr std::size_t kPresetNameCapacity = 192;
inline constexpr std::size_t kPresetFileNameCapacity = 1024;
inline constexpr std::string_view kDefaultPresetFileName = "SummonLab-presets.json";
// Matches the host's anomaly.config document limit.
inline constexpr std::size_t kMaximumSettingsBytes = 1024 * 1024;
inline constexpr std::string_view kSettingsSchemaId = "summon-settings";
inline constexpr std::uint32_t kSettingsSchemaVersion = 1;

struct BirthPreset {
    int enabled{};
    double value{};
};

// Captured on submission; later UI edits only affect the next batch.
struct SummonSettings {
    std::string class_path;
    std::string display_name;
    std::string combat_config_blueprint; // Optional same-family source, selected by the user.
    std::string advanced_combat_blueprint;
    bool advanced_combat_source{};
    std::string server_combat_blueprint; // Resolved from the active Profile, never imported.
    std::uint32_t level{}, skill_level{};
    bool hostile{}, pet{}, aggressive{}, ignore_player{}, compatibility{};
    bool mute_boss_music{}, mute_boss_dialogue{};
    bool skip_cutscenes{};
    std::array<BirthPreset, kBirthAttributeNames.size()> presets{};
    BlueprintEdits editor;
    std::uint32_t count{1};
    double spacing{5.0};
};

inline std::string_view SelectedCombatBlueprint(const SummonSettings& settings) {
    return settings.advanced_combat_source && !settings.advanced_combat_blueprint.empty()
        ? settings.advanced_combat_blueprint : settings.combat_config_blueprint;
}

struct SummonPreset {
    std::string name;
    SummonSettings settings;
};

struct SavedConfiguration {
    SummonSettings current;
    std::vector<SummonPreset> presets;
    std::map<std::string, BlueprintEdits, std::less<>> editor_blueprints;
    std::string catalog_filter, attribute_filter;
    std::string export_name{kDefaultPresetFileName}, export_folder, import_file;
    bool only_overrides{}, only_current_blueprint{true};
};

inline bool ValidPresetName(std::string_view name) {
    return !name.empty() && name.size() < kPresetNameCapacity &&
        name.find('\0') == std::string_view::npos &&
        name.find_first_not_of(" \t\r\n") != std::string_view::npos;
}

inline bool ValidBlueprintPath(std::string_view path) {
    return path.size() < 512 && path.starts_with("/Game/") && path.ends_with("_C") &&
        path.find('.') != std::string_view::npos && path.find('\0') == std::string_view::npos;
}

inline nlohmann::json EditorJson(const BlueprintEdits& edits) {
    auto fields = nlohmann::json::array();
    for (const auto& field : edits.fields) {
        if (!ValidFieldValue(field.kind, field.value)) throw std::invalid_argument("blueprint field value");
        fields.push_back({{"owner", field.owner}, {"name", field.name},
            {"type", FieldKindName(field.kind)}, {"value", field.value}});
    }
    return {{"fields", std::move(fields)}, {"disabledSkills", edits.disabled_skills},
        {"removedEffects", edits.removed_effects}};
}

inline nlohmann::json SettingsJson(const SummonSettings& value) {
    nlohmann::json attributes = nlohmann::json::object();
    for (std::size_t i = 0; i < value.presets.size(); ++i) {
        const auto& attribute = value.presets[i];
        if (!std::isfinite(attribute.value) || std::abs(attribute.value) > std::numeric_limits<float>::max())
            throw std::invalid_argument("attribute value");
        // Retain edited values even when the override has been unchecked.
        if (attribute.enabled || attribute.value != 0)
            attributes[std::string(kBirthAttributeNames[i])] = {
                {"enabled", attribute.enabled != 0}, {"value", attribute.value}};
    }
    if (!std::isfinite(value.spacing)) throw std::invalid_argument("spacing");
    return {{"blueprint", value.class_path}, {"displayName", value.display_name},
        {"combatConfigBlueprint", value.combat_config_blueprint},
        {"advancedCombatBlueprint", value.advanced_combat_blueprint},
        {"advancedCombatSource", value.advanced_combat_source},
        {"level", value.level}, {"skillLevel", value.skill_level},
        {"hostile", value.hostile}, {"pet", value.pet}, {"aggressive", value.aggressive},
        {"ignorePlayer", value.ignore_player}, {"ftCompatibility", value.compatibility},
        {"muteBossMusic", value.mute_boss_music}, {"muteBossDialogue", value.mute_boss_dialogue},
        {"skipCutscenes", value.skip_cutscenes},
        {"count", value.count}, {"spacing", value.spacing}, {"attributes", std::move(attributes)},
        {"editor", EditorJson(value.editor)}};
}

inline std::string ReadText(const nlohmann::json& value, std::size_t capacity) {
    auto result = value.get<std::string>();
    if (result.size() >= capacity || result.find('\0') != std::string::npos)
        throw std::invalid_argument("text length");
    return result;
}

inline std::uint32_t ReadUnsigned(const nlohmann::json& value, std::uint32_t maximum) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
        throw std::invalid_argument("unsigned integer");
    const auto number = value.get<std::uint64_t>();
    if (number > maximum) throw std::invalid_argument("integer range");
    return static_cast<std::uint32_t>(number);
}

inline void CheckKeys(const nlohmann::json& value, std::initializer_list<std::string_view> keys) {
    if (!value.is_object()) throw std::invalid_argument("expected object");
    for (const auto& [key, unused] : value.items())
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            throw std::invalid_argument("unknown setting");
}

inline BlueprintEdits ReadEditor(const nlohmann::json& json) {
    CheckKeys(json, {"fields", "disabledSkills", "removedEffects"});
    BlueprintEdits result;
    if (!json.at("fields").is_array()) throw std::invalid_argument("blueprint fields");
    for (const auto& item : json.at("fields")) {
        CheckKeys(item, {"owner", "name", "type", "value"});
        BlueprintFieldEdit field;
        field.owner = ReadText(item.at("owner"), 512);
        field.name = ReadText(item.at("name"), 256);
        if (!ValidBlueprintPath(field.owner) || field.name.empty()) throw std::invalid_argument("blueprint field identity");
        const auto type = item.at("type").get<std::string>();
        if (type == "bool") field.kind = BlueprintFieldKind::Boolean;
        else if (type == "int") field.kind = BlueprintFieldKind::Integer;
        else if (type == "float") field.kind = BlueprintFieldKind::Float;
        else if (type == "double") field.kind = BlueprintFieldKind::Double;
        else throw std::invalid_argument("blueprint field type");
        field.value = item.at("value").get<double>();
        if (!ValidFieldValue(field.kind, field.value) || std::any_of(result.fields.begin(), result.fields.end(),
                [&](const auto& other) { return other.owner == field.owner && other.name == field.name; }))
            throw std::invalid_argument("blueprint field value or duplicate");
        result.fields.push_back(std::move(field));
    }
    const auto paths = [](const nlohmann::json& items) {
        if (!items.is_array()) throw std::invalid_argument("blueprint class list");
        std::vector<std::string> result;
        for (const auto& item : items) {
            auto path = ReadText(item, 512);
            if (!ValidBlueprintPath(path) || std::find(result.begin(), result.end(), path) != result.end())
                throw std::invalid_argument("blueprint class identity");
            result.push_back(std::move(path));
        }
        return result;
    };
    result.disabled_skills = paths(json.at("disabledSkills"));
    result.removed_effects = paths(json.at("removedEffects"));
    return result;
}

inline SummonSettings ReadSettings(const nlohmann::json& json) {
    CheckKeys(json, {"blueprint", "displayName", "level", "skillLevel", "hostile", "pet", "aggressive",
        "ignorePlayer", "ftCompatibility", "muteBossMusic", "muteBossDialogue", "count", "spacing", "attributes",
        "combatConfigBlueprint", "advancedCombatBlueprint", "advancedCombatSource", "editor", "skipCutscenes"});
    SummonSettings result;
    result.class_path = ReadText(json.at("blueprint"), 512);
    result.display_name = ReadText(json.at("displayName"), 96);
    if (json.contains("combatConfigBlueprint")) {
        result.combat_config_blueprint = ReadText(json.at("combatConfigBlueprint"), 512);
        if (!result.combat_config_blueprint.empty() && !ValidBlueprintPath(result.combat_config_blueprint))
            throw std::invalid_argument("combat config blueprint");
    }
    if (json.contains("advancedCombatBlueprint")) {
        result.advanced_combat_blueprint = ReadText(json.at("advancedCombatBlueprint"), 512);
        if (!result.advanced_combat_blueprint.empty() && !ValidBlueprintPath(result.advanced_combat_blueprint))
            throw std::invalid_argument("advanced combat blueprint");
    }
    result.advanced_combat_source = json.value("advancedCombatSource", false);
    if (json.contains("editor")) result.editor = ReadEditor(json.at("editor"));
    result.level = ReadUnsigned(json.at("level"), kMaximumLevel);
    result.skill_level = ReadUnsigned(json.at("skillLevel"), kMaximumLevel);
    result.hostile = json.at("hostile").get<bool>();
    result.pet = json.at("pet").get<bool>();
    result.aggressive = json.at("aggressive").get<bool>();
    result.ignore_player = json.at("ignorePlayer").get<bool>();
    result.compatibility = json.at("ftCompatibility").get<bool>();
    result.mute_boss_music = json.at("muteBossMusic").get<bool>();
    result.mute_boss_dialogue = json.at("muteBossDialogue").get<bool>();
    result.skip_cutscenes = json.value("skipCutscenes", false);
    result.count = ReadUnsigned(json.at("count"), std::numeric_limits<std::uint32_t>::max());
    result.spacing = json.at("spacing").get<double>();
    if (result.count == 0 || !std::isfinite(result.spacing) || result.spacing < 0 || result.spacing > 100)
        throw std::invalid_argument("batch settings");
    const auto& attributes = json.at("attributes");
    if (!attributes.is_object()) throw std::invalid_argument("attributes object");
    for (const auto& [name, data] : attributes.items()) {
        const auto found = std::find(kBirthAttributeNames.begin(), kBirthAttributeNames.end(), name);
        if (found == kBirthAttributeNames.end()) throw std::invalid_argument("unknown attribute");
        CheckKeys(data, {"enabled", "value"});
        auto& attribute = result.presets[static_cast<std::size_t>(found - kBirthAttributeNames.begin())];
        attribute.enabled = data.at("enabled").get<bool>() ? 1 : 0;
        attribute.value = data.at("value").get<double>();
        if (!std::isfinite(attribute.value) || std::abs(attribute.value) > std::numeric_limits<float>::max())
            throw std::invalid_argument("attribute range");
    }
    return result;
}

inline nlohmann::json PresetsJson(const std::vector<SummonPreset>& presets) {
    auto result = nlohmann::json::array();
    for (const auto& preset : presets) {
        if (!ValidPresetName(preset.name) || !ValidBlueprintPath(preset.settings.class_path))
            throw std::invalid_argument("preset identity");
        result.push_back({{"name", preset.name}, {"settings", SettingsJson(preset.settings)}});
    }
    return result;
}

inline std::vector<SummonPreset> ReadPresets(const nlohmann::json& json) {
    if (!json.is_array()) throw std::invalid_argument("preset list");
    std::vector<SummonPreset> result;
    for (const auto& entry : json) {
        CheckKeys(entry, {"name", "settings"});
        SummonPreset preset{ReadText(entry.at("name"), kPresetNameCapacity), ReadSettings(entry.at("settings"))};
        if (!ValidPresetName(preset.name) || !ValidBlueprintPath(preset.settings.class_path))
            throw std::invalid_argument("preset identity");
        result.push_back(std::move(preset));
    }
    return result;
}

inline std::string BoundedDocument(const nlohmann::json& json) {
    auto text = json.dump(2);
    if (text.size() > kMaximumSettingsBytes) throw std::length_error("configuration size");
    return text;
}

inline std::string ConfigurationJson(const SavedConfiguration& value) {
    auto editors = nlohmann::json::object();
    for (const auto& [path, edits] : value.editor_blueprints)
        if (!edits.empty()) editors[path] = EditorJson(edits);
    return BoundedDocument({{"schemaVersion", kSettingsSchemaVersion}, {"current", SettingsJson(value.current)},
        {"presets", PresetsJson(value.presets)}, {"editorBlueprints", std::move(editors)}, {"ui", {
            {"catalogFilter", value.catalog_filter}, {"attributeFilter", value.attribute_filter},
            {"onlyOverrides", value.only_overrides}, {"onlyCurrentBlueprint", value.only_current_blueprint},
            {"exportName", value.export_name}, {"exportFolder", value.export_folder},
            {"importFile", value.import_file}}}});
}

inline bool ParseConfiguration(std::string_view text, SavedConfiguration& output) noexcept {
    try {
        if (text.size() > kMaximumSettingsBytes) return false;
        const auto json = nlohmann::json::parse(text);
        CheckKeys(json, {"schemaVersion", "current", "presets", "ui", "editorBlueprints"});
        if (ReadUnsigned(json.at("schemaVersion"), kSettingsSchemaVersion) != kSettingsSchemaVersion) return false;
        SavedConfiguration parsed;
        parsed.current = ReadSettings(json.at("current"));
        parsed.presets = ReadPresets(json.at("presets"));
        if (json.contains("editorBlueprints")) {
            const auto& editors = json.at("editorBlueprints");
            if (!editors.is_object()) return false;
            for (const auto& [path, edits] : editors.items()) {
                if (!ValidBlueprintPath(path)) return false;
                parsed.editor_blueprints.emplace(path, ReadEditor(edits));
            }
        }
        const auto& ui = json.at("ui");
        CheckKeys(ui, {"catalogFilter", "attributeFilter", "onlyOverrides", "onlyCurrentBlueprint",
            "exportName", "exportFolder", "importFile"});
        parsed.catalog_filter = ReadText(ui.at("catalogFilter"), 64);
        parsed.attribute_filter = ReadText(ui.at("attributeFilter"), 64);
        parsed.only_overrides = ui.at("onlyOverrides").get<bool>();
        parsed.only_current_blueprint = ui.at("onlyCurrentBlueprint").get<bool>();
        parsed.export_name = ReadText(ui.at("exportName"), kPresetFileNameCapacity);
        parsed.export_folder = ReadText(ui.at("exportFolder"), kMaximumSettingsBytes);
        parsed.import_file = ReadText(ui.at("importFile"), kMaximumSettingsBytes);
        output = std::move(parsed);
        return true;
    } catch (...) { return false; }
}

inline std::string ExportPresets(const std::vector<SummonPreset>& presets) {
    return BoundedDocument({{"schemaVersion", kSettingsSchemaVersion},
        {"format", "anomaly.summon-lab.presets"}, {"presets", PresetsJson(presets)}});
}

inline bool ImportPresets(std::string_view text, std::vector<SummonPreset>& output) noexcept {
    try {
        if (text.size() > kMaximumSettingsBytes) return false;
        const auto json = nlohmann::json::parse(text);
        CheckKeys(json, {"schemaVersion", "format", "presets"});
        if (json.at("format") != "anomaly.summon-lab.presets" ||
            ReadUnsigned(json.at("schemaVersion"), kSettingsSchemaVersion) != kSettingsSchemaVersion) return false;
        auto parsed = ReadPresets(json.at("presets"));
        if (parsed.empty()) return false;
        output = std::move(parsed);
        return true;
    } catch (...) { return false; }
}

inline std::string ConfigurationSchema() {
    using Json = nlohmann::json;
    const Json boolean{{"type", "boolean"}};
    const Json text{{"type", "string"}};
    Json attributes = Json::object();
    for (const auto name : kBirthAttributeNames)
        attributes[std::string(name)] = {{"type", "object"}, {"additionalProperties", false},
            {"required", {"enabled", "value"}}, {"properties", {{"enabled", boolean},
                {"value", {{"type", "number"}, {"minimum", -std::numeric_limits<float>::max()},
                    {"maximum", std::numeric_limits<float>::max()}}}}}};
    const Json level{{"type", "integer"}, {"minimum", 0}, {"maximum", kMaximumLevel}};
    const Json path{{"type", "string"}, {"maxLength", 511}};
    const Json class_list{{"type", "array"}, {"items", path}, {"uniqueItems", true}};
    const Json editor{{"type", "object"}, {"additionalProperties", false},
        {"required", {"fields", "disabledSkills", "removedEffects"}},
        {"properties", {{"disabledSkills", class_list}, {"removedEffects", class_list},
            {"fields", {{"type", "array"}, {"items", {{"type", "object"}, {"additionalProperties", false},
                {"required", {"owner", "name", "type", "value"}},
                {"properties", {{"owner", path}, {"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 255}}},
                    {"type", {{"type", "string"}, {"enum", {"bool", "int", "float", "double"}}}},
                    {"value", {{"type", "number"}}}}}}}}}}}};
    const Json settings{{"type", "object"}, {"additionalProperties", false},
        {"required", {"blueprint", "displayName", "level", "skillLevel", "hostile", "pet", "aggressive",
            "ignorePlayer", "ftCompatibility", "muteBossMusic", "muteBossDialogue", "count", "spacing", "attributes"}},
        {"properties", {{"blueprint", {{"type", "string"}, {"maxLength", 511}}},
            {"combatConfigBlueprint", {{"type", "string"}, {"maxLength", 511}}},
            {"advancedCombatBlueprint", {{"type", "string"}, {"maxLength", 511}}},
            {"advancedCombatSource", boolean},
            {"editor", editor},
            {"displayName", {{"type", "string"}, {"maxLength", 95}}}, {"level", level}, {"skillLevel", level},
            {"hostile", boolean}, {"pet", boolean}, {"aggressive", boolean}, {"ignorePlayer", boolean},
            {"ftCompatibility", boolean}, {"muteBossMusic", boolean}, {"muteBossDialogue", boolean},
            {"skipCutscenes", boolean},
            {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<std::uint32_t>::max()}}},
            {"spacing", {{"type", "number"}, {"minimum", 0}, {"maximum", 100}}},
            {"attributes", {{"type", "object"}, {"additionalProperties", false}, {"properties", attributes}}}}}};
    return Json{{"type", "object"}, {"additionalProperties", false},
        {"required", {"schemaVersion", "current", "presets", "ui"}},
        {"properties", {{"schemaVersion", {{"type", "integer"}, {"enum", {kSettingsSchemaVersion}}}},
            {"current", {{"$ref", "#/definitions/settings"}}},
            {"editorBlueprints", {{"type", "object"}, {"additionalProperties", editor}}},
            {"presets", {{"type", "array"}, {"items", {
                {"type", "object"}, {"additionalProperties", false}, {"required", {"name", "settings"}},
                {"properties", {{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", kPresetNameCapacity - 1}}},
                    {"settings", {{"$ref", "#/definitions/settings"}}}}}}}}},
            {"ui", {{"type", "object"}, {"additionalProperties", false},
                {"required", {"catalogFilter", "attributeFilter", "onlyOverrides", "onlyCurrentBlueprint",
                    "exportName", "exportFolder", "importFile"}},
                {"properties", {{"catalogFilter", text}, {"attributeFilter", text}, {"onlyOverrides", boolean},
                    {"onlyCurrentBlueprint", boolean},
                    {"exportName", {{"type", "string"}, {"maxLength", kPresetFileNameCapacity - 1}}},
                    {"exportFolder", text}, {"importFile", text}}}}}}},
        {"definitions", {{"settings", settings}}}}.dump();
}

} // namespace summon_lab
