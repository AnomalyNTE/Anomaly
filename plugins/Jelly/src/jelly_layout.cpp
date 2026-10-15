#include "jelly_layout.hpp"

#include "jelly_json.hpp"

#include <cctype>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace jelly {
namespace {

std::string Text(const json::Value* value, const std::string_view key) {
    if (value == nullptr) return {};
    const json::Value* member = value->Find(key);
    if (member == nullptr || !member->IsString()) return {};
    return member->text;
}

std::int64_t Number(const json::Value* value, const std::string_view key, const std::int64_t fallback) {
    if (value == nullptr) return fallback;
    const json::Value* member = value->Find(key);
    if (member == nullptr || !member->IsNumber()) return fallback;
    return static_cast<std::int64_t>(member->number);
}

bool HasMember(const json::Value* value, const std::string_view key) {
    return value != nullptr && value->Find(key) != nullptr;
}

bool ParseHexBytes(const std::string_view text, std::vector<std::uint8_t>& out) {
    out.clear();
    int high = -1;
    for (const char character : text) {
        if (character == ' ' || character == '\t' || character == '\n') continue;
        int digit = -1;
        if (character >= '0' && character <= '9') {
            digit = character - '0';
        } else if (character >= 'a' && character <= 'f') {
            digit = character - 'a' + 10;
        } else if (character >= 'A' && character <= 'F') {
            digit = character - 'A' + 10;
        } else {
            return false;
        }
        if (high < 0) {
            high = digit;
        } else {
            out.push_back(static_cast<std::uint8_t>((high << 4) | digit));
            high = -1;
        }
    }
    return high < 0 && !out.empty();
}

}  // namespace

bool ParseLayout(
    const std::string_view document, Layout& out, std::string& error) noexcept {
    out = Layout{};
    json::Value root;
    if (!json::Parse(document, root, error)) return false;
    if (!root.IsObject()) {
        error = "the layout resource is not an object";
        return false;
    }
    if (Text(&root, "schema") != kLayoutSchemaId) {
        error = "the layout resource has an unexpected schema";
        return false;
    }
    out.version = Number(&root, "version", 0);
    out.game = Text(&root, "game");
    if (const json::Value* source = root.Find("source"); source != nullptr) {
        out.source_note = Text(source, "note");
    }

    if (const json::Value* symbols = root.Find("symbols"); symbols != nullptr) {
        for (const auto& member : symbols->members) {
            const json::Value& entry = member.second;
            SymbolSpec spec;
            spec.type = Text(&entry, "type");
            spec.source = Text(&entry, "source");
            spec.module = Text(&entry, "module");
            spec.section = Text(&entry, "section");
            spec.pattern = Text(&entry, "pattern");
            spec.resolve = Text(&entry, "resolve");
            spec.resolve_offset = Number(&entry, "resolveOffset", 0);
            spec.instruction_size = Number(&entry, "instructionSize", 0);
            spec.addend = Number(&entry, "addend", 0);
            spec.detour_bytes = Number(&entry, "detourBytes", 0);
            if (entry.Find("bytes") != nullptr) {
                static_cast<void>(ParseHexBytes(Text(&entry, "bytes"), out.process_event_prologue));
            }
            if (member.first == "process_event") {
                out.process_event_detour_bytes = spec.detour_bytes;
            }
            spec.present = HasMember(&entry, "pattern");
            out.symbols.emplace(member.first, std::move(spec));
        }
    }

    if (const json::Value* names = root.Find("classNames"); names != nullptr) {
        for (const auto& member : names->members) {
            if (member.second.IsString()) out.class_names.emplace(member.first, member.second.text);
        }
    }

    if (const json::Value* families = root.Find("classFamilies"); families != nullptr) {
        for (const auto& member : families->members) {
            if (!member.second.IsArray()) continue;
            std::vector<std::string> values;
            for (const auto& item : member.second.items) {
                if (item.IsString()) values.push_back(item.text);
            }
            out.class_families.emplace(member.first, std::move(values));
        }
    }

    if (const json::Value* offsets = root.Find("offsets"); offsets != nullptr) {
        for (const auto& member : offsets->members) {
            const json::Value& entry = member.second;
            OffsetEntry record;
            record.value = Number(&entry, "value", 0);
            record.type = Text(&entry, "type");
            record.source = Text(&entry, "source");
            record.present = entry.IsNumber() || HasMember(&entry, "value");
            out.offsets.emplace(member.first, std::move(record));
        }
    }

    if (const json::Value* properties = root.Find("properties"); properties != nullptr) {
        for (const auto& member : properties->members) {
            const json::Value& entry = member.second;
            PropertySpec spec;
            spec.owner = Text(&entry, "owner");
            spec.name = Text(&entry, "name");
            spec.type = Text(&entry, "type");
            spec.struct_name = Text(&entry, "struct_name");
            spec.class_name = Text(&entry, "class_name");
            spec.inner_type = Text(&entry, "inner_type");
            spec.inner_class_name = Text(&entry, "inner_class_name");
            spec.source = Text(&entry, "source");
            spec.element_size = Number(&entry, "element_size", 0);
            spec.present = !spec.name.empty() && !spec.owner.empty();
            out.properties.emplace(member.first, std::move(spec));
        }
    }

    if (const json::Value* functions = root.Find("functions"); functions != nullptr) {
        for (const auto& member : functions->members) {
            if (!member.second.IsArray()) continue;
            std::vector<FunctionSpec> candidates;
            for (const auto& entry : member.second.items) {
                FunctionSpec spec;
                spec.owner = Text(&entry, "owner");
                spec.name = Text(&entry, "name");
                spec.source = Text(&entry, "source");
                spec.num_parms = static_cast<std::uint32_t>(Number(&entry, "numParms", 0));
                spec.parms_size = static_cast<std::uint32_t>(Number(&entry, "parmsSize", 0));
                spec.return_value_offset =
                    static_cast<std::uint32_t>(Number(&entry, "returnValueOffset", 0));
                spec.exact_size = spec.parms_size != 0;
                if (const json::Value* params = entry.Find("params");
                    params != nullptr && params->IsArray()) {
                    for (const auto& item : params->items) {
                        FunctionParamSpec param;
                        param.name = Text(&item, "name");
                        param.type = Text(&item, "type");
                        param.struct_name = Text(&item, "struct_name");
                        param.element_size = Number(&item, "element_size", 0);
                        param.offset = Number(&item, "offset", 0);
                        spec.params.push_back(std::move(param));
                    }
                }
                if (!spec.name.empty() && !spec.owner.empty()) {
                    candidates.push_back(std::move(spec));
                }
            }
            out.functions.emplace(member.first, std::move(candidates));
        }
    }
    return true;
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
        if (found->second.source.empty() || found->second.type.empty()) {
            problems.emplace_back(
                "offset " + std::string(key) + " does not record its source and type");
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
        } else if (found->second.source.empty()) {
            problems.emplace_back(
                "property " + std::string(key) + " does not record its source");
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
