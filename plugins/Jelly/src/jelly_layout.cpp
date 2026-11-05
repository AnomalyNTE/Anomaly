#include "jelly_layout.hpp"
#include <cctype>
#include <stdexcept>
#include <utility>

namespace jelly {
Layout FromProfile(const anomaly::sdk::Profile& profile) {
    if (!profile.Matches({"nte", "HTGame.exe", ".text", "jelly-reflection-v1"}))
        throw std::runtime_error("unsupported Jelly profile contract");
    const auto value = [&](std::string_view key) {
        return profile.Value(key, 65536);
    };
    Layout out;
    out.version = 1;
    out.game = profile.game;
    out.offsets.emplace("object.internal_index", OffsetEntry{value("object.internal_index"), "int32", {}, true});
    out.offsets.emplace("object.class", OffsetEntry{value("object.class"), "UClass*", {}, true});
    out.offsets.emplace("object.name_offset", OffsetEntry{value("object.name_offset"), "FName id (uint32)", {}, true});
    out.offsets.emplace("object.outer", OffsetEntry{value("object.outer"), "UObject*", {}, true});
    out.offsets.emplace("ustruct.super_struct", OffsetEntry{value("ustruct.super_struct"), "UStruct*", {}, true});
    out.offsets.emplace("ustruct.children", OffsetEntry{value("ustruct.children"), "UField* head of the UField chain", {}, true});
    out.offsets.emplace("ustruct.property_link", OffsetEntry{value("ustruct.property_link"), "FField* head of the child-property chain", {}, true});
    out.offsets.emplace("ufield.next", OffsetEntry{value("ufield.next"), "UField*", {}, true});
    out.offsets.emplace("ffield.next", OffsetEntry{value("ffield.next"), "FField*", {}, true});
    out.offsets.emplace("ffield.name", OffsetEntry{value("ffield.name"), "FName id (uint32)", {}, true});
    out.offsets.emplace("ffield.class", OffsetEntry{value("ffield.class"), "FFieldClass*", {}, true});
    out.offsets.emplace("ffield_class.name", OffsetEntry{value("ffield_class.name"), "FName id (uint32)", {}, true});
    out.offsets.emplace("fproperty.array_dim", OffsetEntry{value("fproperty.array_dim"), "int32", {}, true});
    out.offsets.emplace("fproperty.element_size", OffsetEntry{value("fproperty.element_size"), "int32", {}, true});
    out.offsets.emplace("fproperty.offset_internal", OffsetEntry{value("fproperty.offset_internal"), "int32", {}, true});
    out.offsets.emplace("fstruct_property.struct", OffsetEntry{value("fstruct_property.struct"), "UStruct*", {}, true});
    out.offsets.emplace("fobject_property.property_class", OffsetEntry{value("fobject_property.property_class"), "UClass*", {}, true});
    out.offsets.emplace("farray_property.inner", OffsetEntry{value("farray_property.inner"), "FProperty*", {}, true});
    out.offsets.emplace("fbool_property.field_size", OffsetEntry{value("fbool_property.field_size"), "uint8", {}, true});
    out.offsets.emplace("fbool_property.byte_offset", OffsetEntry{value("fbool_property.byte_offset"), "uint8", {}, true});
    out.offsets.emplace("fbool_property.byte_mask", OffsetEntry{value("fbool_property.byte_mask"), "uint8", {}, true});
    out.offsets.emplace("ufunction.num_parms", OffsetEntry{value("ufunction.num_parms"), "uint8", {}, true});
    out.offsets.emplace("ufunction.parms_size", OffsetEntry{value("ufunction.parms_size"), "uint16", {}, true});
    out.offsets.emplace("ufunction.return_value_offset", OffsetEntry{value("ufunction.return_value_offset"), "uint16", {}, true});
    out.offsets.emplace("objects.items", OffsetEntry{value("objects.items"), "FUObjectItem** chunk table", {}, true});
    out.offsets.emplace("objects.count", OffsetEntry{value("objects.count"), "int32", {}, true});
    out.offsets.emplace("objects.max_count", OffsetEntry{value("objects.max_count"), "int32", {}, true});
    out.offsets.emplace("objects.num_chunks", OffsetEntry{value("objects.num_chunks"), "uint32", {}, true});
    out.offsets.emplace("objects.max_chunks", OffsetEntry{value("objects.max_chunks"), "uint32", {}, true});
    out.offsets.emplace("objects.chunk_count_size", OffsetEntry{value("objects.chunk_count_size"), "field width of the chunk counters", {}, true});
    out.offsets.emplace("objects.chunk_size", OffsetEntry{value("objects.chunk_size"), "objects per chunk", {}, true});
    out.offsets.emplace("objects.item_stride", OffsetEntry{value("objects.item_stride"), "FUObjectItem stride", {}, true});
    out.offsets.emplace("objects.object_offset", OffsetEntry{value("objects.object_offset"), "UObject* offset inside FUObjectItem", {}, true});
    out.offsets.emplace("objects.serial_offset", OffsetEntry{value("objects.serial_offset"), "int32 serial offset inside FUObjectItem", {}, true});
    out.offsets.emplace("tarray.data", OffsetEntry{value("tarray.data"), "pointer to the element array", {}, true});
    out.offsets.emplace("tarray.num", OffsetEntry{value("tarray.num"), "int32 element count", {}, true});
    out.offsets.emplace("tarray.max", OffsetEntry{value("tarray.max"), "int32 allocation capacity", {}, true});
    out.class_names.emplace("actor", "Actor");
    out.class_names.emplace("scene_component", "SceneComponent");
    out.class_names.emplace("primitive_component", "PrimitiveComponent");
    out.class_names.emplace("mesh_component", "MeshComponent");
    out.class_names.emplace("character", "Character");
    out.class_names.emplace("function", "Function");
    out.class_names.emplace("vector_struct", "Vector");
    out.class_names.emplace("body_instance_struct", "BodyInstance");
    out.class_families.emplace("instanced_mesh", std::vector<std::string>{"InstancedStaticMeshComponent", "HierarchicalInstancedStaticMeshComponent"});
    out.class_families.emplace("non_visual_mesh", std::vector<std::string>{"WidgetComponent"});
    out.class_families.emplace("vehicle", std::vector<std::string>{"BP_Vehicle"});
    {
        PropertySpec spec;
        spec.owner = "Actor";
        spec.name = "RootComponent";
        spec.type = "ObjectProperty";
        spec.class_name = "SceneComponent";
        spec.present = true;
        out.properties.emplace("root_component", std::move(spec));
    }
    {
        PropertySpec spec;
        spec.owner = "SceneComponent";
        spec.name = "AttachChildren";
        spec.type = "ArrayProperty";
        spec.inner_type = "ObjectProperty";
        spec.inner_class_name = "SceneComponent";
        spec.present = true;
        out.properties.emplace("attach_children", std::move(spec));
    }
    {
        PropertySpec spec;
        spec.owner = "SceneComponent";
        spec.name = "AttachParent";
        spec.type = "ObjectProperty";
        spec.class_name = "SceneComponent";
        spec.present = true;
        out.properties.emplace("attach_parent", std::move(spec));
    }
    {
        PropertySpec spec;
        spec.owner = "SceneComponent";
        spec.name = "RelativeScale3D";
        spec.type = "StructProperty";
        spec.struct_name = "Vector";
        spec.element_size = value("property.relative_scale.element_size");
        spec.present = true;
        out.properties.emplace("relative_scale", std::move(spec));
    }
    {
        PropertySpec spec;
        spec.owner = "SceneComponent";
        spec.name = "RelativeRotation";
        spec.type = "StructProperty";
        spec.struct_name = "Rotator";
        spec.element_size = value("property.relative_rotation.element_size");
        spec.present = true;
        out.properties.emplace("relative_rotation", std::move(spec));
    }
    {
        PropertySpec spec;
        spec.owner = "SceneComponent";
        spec.name = "Mobility";
        spec.element_size = value("property.mobility.element_size");
        spec.present = true;
        out.properties.emplace("mobility", std::move(spec));
    }
    {
        PropertySpec spec;
        spec.owner = "SceneComponent";
        spec.name = "bVisible";
        spec.type = "BoolProperty";
        spec.present = true;
        out.properties.emplace("visible", std::move(spec));
    }
    {
        PropertySpec spec;
        spec.owner = "Actor";
        spec.name = "bHidden";
        spec.type = "BoolProperty";
        spec.element_size = value("property.hidden.element_size");
        spec.present = true;
        out.properties.emplace("hidden", std::move(spec));
    }
    {
        PropertySpec spec;
        spec.owner = "PrimitiveComponent";
        spec.name = "BodyInstance";
        spec.type = "StructProperty";
        spec.struct_name = "BodyInstance";
        spec.present = true;
        out.properties.emplace("body_instance", std::move(spec));
    }
    {
        PropertySpec spec;
        spec.owner = "BodyInstance";
        spec.name = "CollisionEnabled";
        spec.element_size = value("property.collision_enabled.element_size");
        spec.present = true;
        out.properties.emplace("collision_enabled", std::move(spec));
    }
    {
        PropertySpec spec;
        spec.owner = "BodyInstance";
        spec.name = "bSimulatePhysics";
        spec.type = "BoolProperty";
        spec.present = true;
        out.properties.emplace("simulate_physics", std::move(spec));
    }
    {
        FunctionSpec spec;
        spec.owner = "SceneComponent";
        spec.name = "SetRelativeScale3D";
        spec.num_parms = value("function.set_relative_scale.0.numParms");
        spec.parms_size = value("function.set_relative_scale.0.parmsSize");
        spec.return_value_offset = value("function.set_relative_scale.0.returnValueOffset");
        spec.exact_size = spec.parms_size != 0;
        spec.params.push_back({"NewScale3D", "StructProperty", "Vector", value("function.set_relative_scale.0.param0.size"), value("function.set_relative_scale.0.param0.offset")});
        out.functions["set_relative_scale"].push_back(std::move(spec));
    }
    {
        FunctionSpec spec;
        spec.owner = "SceneComponent";
        spec.name = "K2_SetRelativeScale3D";
        spec.num_parms = value("function.set_relative_scale.1.numParms");
        spec.parms_size = value("function.set_relative_scale.1.parmsSize");
        spec.return_value_offset = value("function.set_relative_scale.1.returnValueOffset");
        spec.exact_size = spec.parms_size != 0;
        spec.params.push_back({"NewScale3D", "StructProperty", "Vector", value("function.set_relative_scale.1.param0.size"), value("function.set_relative_scale.1.param0.offset")});
        out.functions["set_relative_scale"].push_back(std::move(spec));
    }
    {
        FunctionSpec spec;
        spec.owner = "SceneComponent";
        spec.name = "K2_SetRelativeRotation";
        spec.num_parms = value("function.set_relative_rotation.0.numParms");
        spec.parms_size = value("function.set_relative_rotation.0.parmsSize");
        spec.return_value_offset = value("function.set_relative_rotation.0.returnValueOffset");
        spec.exact_size = spec.parms_size != 0;
        spec.params.push_back({"NewRotation", "StructProperty", "Rotator", value("function.set_relative_rotation.0.param0.size"), value("function.set_relative_rotation.0.param0.offset")});
        spec.params.push_back({"bSweep", "BoolProperty", "", value("function.set_relative_rotation.0.param1.size"), value("function.set_relative_rotation.0.param1.offset")});
        spec.params.push_back({"SweepHitResult", "StructProperty", "HitResult", value("function.set_relative_rotation.0.param2.size"), value("function.set_relative_rotation.0.param2.offset")});
        spec.params.push_back({"bTeleport", "BoolProperty", "", value("function.set_relative_rotation.0.param3.size"), value("function.set_relative_rotation.0.param3.offset")});
        out.functions["set_relative_rotation"].push_back(std::move(spec));
    }
    for (const auto& symbol : profile.symbols) {
        if (symbol.candidates.size() != 1) throw std::runtime_error("unsupported Jelly symbol contract");
        const auto& candidate = symbol.candidates.front();
        SymbolSpec spec;
        spec.pattern = candidate.pattern;
        spec.module = profile.module;
        spec.section = profile.section;
        spec.present = true;
        if (symbol.name == "process_event") {
            spec.detour_bytes = candidate.values.at("detourBytes");
            out.process_event_detour_bytes = spec.detour_bytes;
        }
        if (symbol.name == "process_event_prologue") out.process_event_prologue = candidate.entry;
        out.symbols.emplace(symbol.name, std::move(spec));
    }
    const auto problems = LayoutProblems(out);
    if (!problems.empty()) throw std::runtime_error(problems.front());
    return out;
}

bool DropLeadingBytes(
    const std::string_view pattern, const std::int64_t skip, std::string& out) {
    out.clear();
    if (skip <= 0) return false;
    std::vector<std::string_view> tokens;
    std::size_t cursor{};
    while (cursor < pattern.size()) {
        while (cursor < pattern.size() &&
               std::isspace(static_cast<unsigned char>(pattern[cursor])) != 0) {
            ++cursor;
        }
        if (cursor == pattern.size()) break;
        std::size_t end = cursor;
        while (end < pattern.size() &&
               std::isspace(static_cast<unsigned char>(pattern[end])) == 0) {
            ++end;
        }
        tokens.push_back(pattern.substr(cursor, end - cursor));
        cursor = end;
    }
    const auto dropped = static_cast<std::size_t>(skip);
    if (tokens.size() <= dropped) return false;
    const auto whole_byte = [](const std::string_view token) {
        if (token.size() != 2) return false;
        for (const char character : token) {
            if (std::isxdigit(static_cast<unsigned char>(character)) == 0) return false;
        }
        return true;
    };
    for (std::size_t index = 0; index < dropped; ++index) {
        if (!whole_byte(tokens[index])) return false;
    }
    for (std::size_t index = dropped; index < tokens.size(); ++index) {
        if (!out.empty()) out.push_back(' ');
        out.append(tokens[index]);
    }
    return true;
}

std::vector<std::string> LayoutProblems(const Layout& layout) {
    std::vector<std::string> problems;
    const auto require_offset = [&problems, &layout](const std::string_view key) {
        const auto found = layout.offsets.find(key);
        if (found == layout.offsets.end() || !found->second.present) {
            problems.emplace_back("missing offset " + std::string(key));
            return;
        }
        if (found->second.type.empty()) {
            problems.emplace_back(
                "offset " + std::string(key) + " does not record its type");
        }
    };
    const auto require_symbol = [&problems, &layout](const std::string_view key) {
        const auto found = layout.symbols.find(key);
        if (found == layout.symbols.end() || !found->second.present) {
            problems.emplace_back("missing symbol " + std::string(key));
        }
    };
    const auto require_name = [&problems, &layout](const std::string_view key) {
        if (layout.class_names.find(key) == layout.class_names.end()) {
            problems.emplace_back("missing class name " + std::string(key));
        }
    };
    const auto require_property = [&problems, &layout](const std::string_view key) {
        const auto found = layout.properties.find(key);
        if (found == layout.properties.end() || !found->second.present) {
            problems.emplace_back("missing property " + std::string(key));
        }
    };
    const auto require_function = [&problems, &layout](const std::string_view key) {
        const auto found = layout.functions.find(key);
        if (found == layout.functions.end() || found->second.empty()) {
            problems.emplace_back("missing function " + std::string(key));
        }
    };

    require_offset("object.internal_index");
    require_offset("object.class");
    require_offset("object.name_offset");
    require_offset("object.outer");
    require_offset("ustruct.super_struct");
    require_offset("ustruct.children");
    require_offset("ustruct.property_link");
    require_offset("ufield.next");
    require_offset("ffield.next");
    require_offset("ffield.name");
    require_offset("ffield.class");
    require_offset("ffield_class.name");
    require_offset("fproperty.array_dim");
    require_offset("fproperty.element_size");
    require_offset("fproperty.offset_internal");
    require_offset("fstruct_property.struct");
    require_offset("fobject_property.property_class");
    require_offset("farray_property.inner");
    require_offset("fbool_property.byte_offset");
    require_offset("fbool_property.byte_mask");
    require_offset("ufunction.num_parms");
    require_offset("ufunction.parms_size");
    require_offset("ufunction.return_value_offset");
    require_offset("objects.items");
    require_offset("objects.count");
    require_offset("objects.max_count");
    require_offset("objects.num_chunks");
    require_offset("objects.max_chunks");
    require_offset("objects.chunk_count_size");
    require_offset("objects.chunk_size");
    require_offset("objects.item_stride");
    require_offset("objects.object_offset");
    require_offset("objects.serial_offset");
    require_offset("tarray.data");
    require_offset("tarray.num");
    require_offset("tarray.max");

    require_symbol("gobjects");
    require_symbol("process_event");
    if (const auto found = layout.symbols.find("process_event");
        found != layout.symbols.end() &&
        found->second.detour_bytes != kProcessEventDetourBytes) {
        problems.emplace_back(
            "symbol process_event does not declare the supported detour size");
    }
    if (layout.process_event_prologue.empty()) {
        problems.emplace_back("missing the ProcessEvent prologue bytes");
    } else if (static_cast<std::int64_t>(layout.process_event_prologue.size()) <=
               kProcessEventDetourBytes) {
        problems.emplace_back("the ProcessEvent prologue bytes do not outlast the detour");
    }

    require_name("actor");
    require_name("scene_component");
    require_name("mesh_component");
    require_name("character");
    require_name("function");
    require_name("vector_struct");

    if (const auto found = layout.class_families.find("instanced_mesh");
        found == layout.class_families.end() || found->second.empty()) {
        problems.emplace_back("missing the instanced-mesh class family");
    }
    if (const auto found = layout.class_families.find("non_visual_mesh");
        found == layout.class_families.end() || found->second.empty()) {
        problems.emplace_back("missing the non-visual-mesh class family");
    }
    if (const auto found = layout.class_families.find("vehicle");
        found == layout.class_families.end() || found->second.empty()) {
        problems.emplace_back("missing the vehicle class family");
    }

    require_property("root_component");
    require_property("attach_children");
    require_property("attach_parent");
    require_property("relative_scale");
    require_property("relative_rotation");
    require_property("mobility");
    require_property("visible");
    require_property("hidden");
    require_property("body_instance");
    require_property("collision_enabled");
    require_property("simulate_physics");
    require_function("set_relative_scale");
    require_function("set_relative_rotation");
    return problems;
}

std::int64_t LayoutOffset(
    const Layout& layout, const std::string_view key, const std::int64_t fallback) noexcept {
    const auto found = layout.offsets.find(key);
    return found == layout.offsets.end() || !found->second.present ? fallback
                                                                  : found->second.value;
}

std::string_view LayoutName(const Layout& layout, const std::string_view key) noexcept {
    const auto found = layout.class_names.find(key);
    return found == layout.class_names.end() ? std::string_view{} : std::string_view(found->second);
}

}  // namespace jelly
