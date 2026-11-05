#include "character_attribute_source.hpp"

#include "native_layout.hpp"

#include <anomaly/sdk/cpp.hpp>

#include <array>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

namespace nte::combat_stats {
namespace {

constexpr std::string_view kCharacterTablePath =
    "/Game/DataTable/Character/DT_Character.DT_Character";
constexpr std::string_view kElementFieldName = "CharacterElementType";
constexpr std::string_view kElementStructName = "ElementData";

// ECharacterElementType ordinal order is a game implementation detail the
// exported DataTable data does not record. This order was derived by reading the
// live table and cross-checking 25 characters against their DT_Character names,
// so it is evidence, not a guess.
[[nodiscard]] std::string_view ElementName(const std::int32_t value) noexcept {
    switch (value) {
    case 1: return "COSMOS";
    case 2: return "NATURE";
    case 3: return "INCANTATION";
    case 4: return "CHAOS";
    case 5: return "PSYCHE";
    case 6: return "LAKSHANA";
    default: return {};
    }
}

class Reader {
public:
    explicit Reader(const AnomalyCoreServiceV1* core) : core_(core) {}

    [[nodiscard]] bool Read(const std::uintptr_t address, void* destination,
                            const std::size_t size) const {
        if (core_ == nullptr || core_->read_memory == nullptr || address == 0 ||
            destination == nullptr || size == 0) {
            return false;
        }
        return core_->read_memory(core_->user, address,
                   {static_cast<std::uint8_t*>(destination), size}).code ==
            ANOMALY_STATUS_V1_OK;
    }

    template <typename T>
    [[nodiscard]] bool Value(const std::uintptr_t address, T& out) const {
        return Read(address, &out, sizeof(T));
    }

private:
    const AnomalyCoreServiceV1* core_{};
};

// Two-phase: the Host fills a small buffer directly and reports
// BUFFER_TOO_SMALL only when it does not fit. A sizing call with a null
// destination is not the contract, so try a stack buffer first.
[[nodiscard]] bool ResolveName(const AnomalyUe5NamesServiceV1* names,
                               const std::uint32_t name_id, std::string& out) {
    out.clear();
    if (names == nullptr || names->resolve_utf8 == nullptr || name_id == 0) return false;
    std::array<char, 128> local{};
    std::size_t size = local.size();
    AnomalyStatusV1 status =
        names->resolve_utf8(names->user, name_id, local.data(), &size);
    if (status.code == ANOMALY_STATUS_V1_OK && size > 1 && size <= local.size()) {
        out.assign(local.data(), size - 1U);
        return !out.empty();
    }
    if (status.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL || size <= 1 || size > 512) {
        return false;
    }
    std::string value(size, '\0');
    status = names->resolve_utf8(names->user, name_id, value.data(), &size);
    if (status.code != ANOMALY_STATUS_V1_OK || size <= 1 || size > value.size()) {
        return false;
    }
    value.resize(size - 1U);
    out = std::move(value);
    return !out.empty();
}

struct ObjectRegistry {
    std::uintptr_t items{};
    // These are 32-bit fields in FUObjectArray. Reading them at pointer width
    // would pull each neighbouring field in and fail validation.
    std::uint32_t count{};
    std::uint32_t max_count{};
    std::uint32_t max_chunks{};
    std::uint32_t num_chunks{};
};

[[nodiscard]] bool ResolveRegistry(const Reader& reader,
                                   const std::uintptr_t gobjects_accessor,
                                   const native::Layout& layout,
                                   ObjectRegistry& out) {
    if (gobjects_accessor == 0) return false;
    std::int32_t displacement{};
    if (!reader.Value(gobjects_accessor + native::kRipDisplacementOffset, displacement)) {
        return false;
    }
    const auto resolved = static_cast<std::intptr_t>(gobjects_accessor) +
        static_cast<std::intptr_t>(native::kRipInstructionSize) + displacement;
    const auto base = static_cast<std::uintptr_t>(resolved + layout.gobjects_addend);
    if (base == 0) return false;
    return reader.Value(base + layout.object_registry_items_offset, out.items) &&
        reader.Value(base + layout.object_count_offset, out.count) &&
        reader.Value(base + layout.object_max_count_offset, out.max_count) &&
        reader.Value(base + layout.object_max_chunks_offset, out.max_chunks) &&
        reader.Value(base + layout.object_num_chunks_offset, out.num_chunks) &&
        out.items != 0 && out.count > 0 && out.count <= out.max_count &&
        out.num_chunks > 0 && out.num_chunks <= out.max_chunks;
}

// Handles encode the registry index in the low 32 bits, one-based.
[[nodiscard]] bool ObjectAddress(const Reader& reader, const native::Layout& layout,
                                 const ObjectRegistry& registry,
                                 const AnomalyGenerationHandleV1 handle,
                                 std::uintptr_t& address) {
    address = 0;
    const std::uint32_t encoded = static_cast<std::uint32_t>(handle.id & 0xFFFFFFFFULL);
    if (encoded == 0) return false;
    const std::uint32_t index = encoded - 1U;
    if (index >= registry.count) return false;
    const std::uint32_t chunk_index = index / layout.object_chunk_size;
    const std::uint32_t within_chunk = index % layout.object_chunk_size;
    if (chunk_index >= registry.num_chunks) return false;
    std::uintptr_t chunk{};
    return reader.Value(registry.items + chunk_index * sizeof(void*), chunk) &&
        chunk != 0 &&
        reader.Value(chunk + within_chunk * layout.object_item_stride, address) &&
        address != 0;
}

struct Field {
    std::int32_t offset{};
    std::int32_t element_size{};
};

// Finds one field on a struct, walking this struct's chain and then its bases.
// ElementData inherits its fields from a parent row struct, so the base walk is
// required rather than defensive.
[[nodiscard]] bool FindField(const Reader& reader,
                             const AnomalyUe5NamesServiceV1* names,
                             const native::Layout& layout,
                             std::uintptr_t structure, const std::string_view wanted,
                             Field& out) {
    for (int depth = 0; structure != 0 && depth < 16; ++depth) {
        std::uintptr_t field{};
        if (!reader.Value(structure + layout.struct_child_properties, field)) return false;
        for (int guard = 0; field != 0 && guard < 2048; ++guard) {
            std::uint32_t name_id{};
            std::uintptr_t next{};
            if (!reader.Value(field + layout.field_name, name_id) ||
                !reader.Value(field + layout.field_next, next)) {
                return false;
            }
            if (next == field) break;
            std::string name;
            if (name_id != 0 && ResolveName(names, name_id, name) && name == wanted) {
                return reader.Value(field + layout.property_element_size, out.element_size) &&
                    reader.Value(field + layout.property_offset_internal, out.offset);
            }
            field = next;
        }
        std::uintptr_t parent{};
        if (!reader.Value(structure + layout.struct_super_struct, parent) ||
            parent == structure) {
            break;
        }
        structure = parent;
    }
    return false;
}

// Finds just a property's offset on a struct chain. Enough for a field whose
// type metadata is not needed, such as the ElementData slot.
[[nodiscard]] bool FindPropertyOffset(const Reader& reader,
                                      const AnomalyUe5NamesServiceV1* names,
                                      const native::Layout& layout,
                                      std::uintptr_t structure,
                                      const std::string_view wanted,
                                      std::int32_t& offset) {
    for (int depth = 0; structure != 0 && depth < 16; ++depth) {
        std::uintptr_t field{};
        if (!reader.Value(structure + layout.struct_child_properties, field)) return false;
        for (int guard = 0; field != 0 && guard < 512; ++guard) {
            std::uint32_t name_id{};
            std::uintptr_t next{};
            if (!reader.Value(field + layout.field_name, name_id) ||
                !reader.Value(field + layout.field_next, next)) {
                return false;
            }
            std::string name;
            if (name_id != 0 && ResolveName(names, name_id, name) && name == wanted) {
                return reader.Value(field + layout.property_offset_internal, offset);
            }
            if (next == field) break;
            field = next;
        }
        std::uintptr_t parent{};
        if (!reader.Value(structure + layout.struct_super_struct, parent) ||
            parent == structure) {
            break;
        }
        structure = parent;
    }
    return false;
}

// Lists the first fields of a struct so a mismatch can be read off one log line
// instead of guessed at.
[[nodiscard]] std::string DescribeFields(const Reader& reader,
                                         const AnomalyUe5NamesServiceV1* names,
                                         const native::Layout& layout,
                                         const std::uintptr_t structure) {
    std::string result;
    std::uintptr_t field{};
    if (structure == 0 ||
        !reader.Value(structure + layout.struct_child_properties, field)) {
        return "<unreadable>";
    }
    for (int guard = 0; field != 0 && guard < 64; ++guard) {
        std::uint32_t name_id{};
        std::uintptr_t next{};
        if (!reader.Value(field + layout.field_name, name_id) ||
            !reader.Value(field + layout.field_next, next)) {
            break;
        }
        std::string name;
        if (name_id != 0 && ResolveName(names, name_id, name)) {
            if (!result.empty()) result += ", ";
            result += name;
        }
        if (next == field) break;
        field = next;
    }
    return result.empty() ? "<empty>" : result;
}

// Formats an address for diagnostics.
[[nodiscard]] std::string Hex(const std::uintptr_t value) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%llX",
        static_cast<unsigned long long>(value));
    return buffer;
}

// Walks the row struct to the named field and reports what it is and what the
// candidate struct-pointer slots hold. One run then pins the layout instead of
// another guess.
[[nodiscard]] std::string DescribeStructProperty(const Reader& reader,
                                                 const AnomalyUe5NamesServiceV1* names,
                                                 const native::Layout& layout,
                                                 const std::uintptr_t structure,
                                                 const std::string_view wanted) {
    std::uintptr_t field{};
    if (!reader.Value(structure + layout.struct_child_properties, field)) {
        return "unreadable chain";
    }
    for (int guard = 0; field != 0 && guard < 512; ++guard) {
        std::uint32_t name_id{};
        std::uintptr_t next{};
        if (!reader.Value(field + layout.field_name, name_id) ||
            !reader.Value(field + layout.field_next, next)) {
            return "unreadable field";
        }
        std::string name;
        if (name_id != 0 && ResolveName(names, name_id, name) && name == wanted) {
            std::uintptr_t field_class{};
            std::uint32_t field_class_name{};
            std::string class_text;
            static_cast<void>(reader.Value(field + layout.field_class, field_class));
            static_cast<void>(ResolveName(names,
                reader.Value(field_class + layout.object_name_offset, field_class_name)
                    ? field_class_name : 0, class_text));
            std::int32_t element_size{};
            std::int32_t offset{};
            static_cast<void>(
                reader.Value(field + layout.property_element_size, element_size));
            static_cast<void>(
                reader.Value(field + layout.property_offset_internal, offset));
            std::string slots;
            for (std::int32_t slot = static_cast<std::int32_t>(
                     layout.diagnostic_slot_start);
                 slot <= static_cast<std::int32_t>(layout.diagnostic_slot_end);
                 slot += static_cast<std::int32_t>(layout.diagnostic_slot_step)) {
                std::uintptr_t value{};
                if (!reader.Value(field + slot, value) || value == 0) continue;
                if (!slots.empty()) slots += ",";
                char piece[128]{};
                // Probe the object name and first child so a slot that really
                // holds a UScriptStruct identifies itself and its first field.
                std::uint32_t object_name{};
                std::string object_text;
                static_cast<void>(ResolveName(names,
                    reader.Value(value + layout.object_name_offset, object_name)
                        ? object_name : 0, object_text));
                std::uintptr_t child{};
                std::string child_text;
                if (reader.Value(value + layout.struct_child_properties, child) &&
                    child != 0) {
                    std::uint32_t child_name{};
                    static_cast<void>(ResolveName(names,
                        reader.Value(child + layout.field_name, child_name)
                            ? child_name : 0, child_text));
                }
                std::snprintf(piece, sizeof(piece), "%02X=0x%llX(%s/%s)", slot,
                    static_cast<unsigned long long>(value),
                    object_text.c_str(), child_text.c_str());
                slots += piece;
            }
            return "class=" + class_text + ", size=" + std::to_string(element_size) +
                ", offset=" + std::to_string(offset) + ", slots=[" + slots + "]";
        }
        if (next == field) break;
        field = next;
    }
    return "field not on chain";
}

[[nodiscard]] std::string RowKey(const std::string_view stem,
                                 const std::uint32_t number) {
    std::string key(stem);
    if (number != 0) {
        key += '_';
        key += std::to_string(number - 1U);
    }
    return key;
}

// Safety bound for the row map walk; not version-dependent compatibility data.
constexpr std::int32_t kMaximumRows = 8192;

}  // namespace

CharacterAttributeSource CharacterAttributeSource::Load(
    const CharacterAttributeServices& services,
    std::string& error) {
    error.clear();
    CharacterAttributeSource result;
    if (services.core == nullptr || services.core->read_memory == nullptr) {
        error = "core.read_memory is unavailable";
        return result;
    }
    if (services.objects == nullptr || services.objects->find_exact == nullptr) {
        error = "ue5.objects.find_exact is unavailable";
        return result;
    }
    if (services.native == nullptr || !services.native->complete) {
        error = "compatibility profile is unavailable: " +
            (services.native != nullptr ? services.native->error : std::string("no snapshot"));
        return result;
    }

    const Reader reader(services.core);
    const native::Layout& layout = services.native->layout;
    ObjectRegistry registry;
    if (!ResolveRegistry(reader, services.native->bindings.gobjects_accessor, layout,
            registry)) {
        error = "the object registry is unreadable with the current profile layout";
        return result;
    }

    AnomalyGenerationHandleV1 handle{};
    if (services.objects->find_exact(services.objects->user,
            anomaly::sdk::StringView(kCharacterTablePath), &handle).code !=
            ANOMALY_STATUS_V1_OK ||
        handle.id == 0) {
        error = "DT_Character is not loaded";
        return result;
    }

    std::uintptr_t table{};
    if (!ObjectAddress(reader, layout, registry, handle, table)) {
        error = "DT_Character has no readable address";
        return result;
    }

    std::uintptr_t row_struct{};
    std::uintptr_t elements{};
    std::int32_t count{};
    std::int32_t max_count{};
    // UDataTable::RowStruct, not UObject::Class: the class pointer names the
    // DataTable type itself, while the row struct is the one whose field chain
    // declares ElementData.
    if (!reader.Value(table + layout.data_table_row_struct, row_struct) ||
        row_struct == 0 ||
        !reader.Value(table + layout.data_table_row_map + layout.row_map_count, count) ||
        !reader.Value(table + layout.data_table_row_map + layout.row_map_max, max_count)) {
        error = "DT_Character carries no row map";
        return result;
    }
    if (count <= 0 || count > kMaximumRows || max_count < count ||
        !reader.Value(table + layout.data_table_row_map, elements) || elements == 0) {
        error = "DT_Character row map is empty";
        return result;
    }

    // ElementData is an FInstancedStruct: the row holds a pointer to the
    // instance, the instance points at its UScriptStruct at +0 and at the
    // payload at kInstancedStructData. The type is chosen per row, so it cannot
    // be resolved from the row struct's metadata; read it from a live instance.
    Field element_field;
    std::int32_t element_data_offset{};
    if (!FindPropertyOffset(reader, services.names, layout, row_struct,
            kElementStructName, element_data_offset)) {
        error = "ElementData is absent from the DT_Character row struct; " +
            DescribeStructProperty(reader, services.names, layout, row_struct,
                kElementStructName);
        return result;
    }

    std::vector<std::pair<std::string, std::uintptr_t>> rows;
    rows.reserve(static_cast<std::size_t>(count));
    for (std::int32_t index = 0; index < count; ++index) {
        const auto element =
            elements + static_cast<std::uintptr_t>(index) * layout.row_stride;
        std::uint32_t key_index{};
        std::uint32_t key_number{};
        std::uintptr_t row{};
        if (!reader.Value(element, key_index) ||
            !reader.Value(element + layout.row_key_number, key_number) ||
            !reader.Value(element + layout.row_data, row) || key_index == 0 || row == 0) {
            continue;
        }
        std::string stem;
        if (!ResolveName(services.names, key_index, stem)) continue;
        rows.emplace_back(RowKey(stem, key_number), row);
    }

    // Take the element struct from the first row whose instance is readable.
    // FInstancedStruct holds {UScriptStruct*, uint8*} at +0 and +8; the type is
    // chosen per row, so it must come from an instance rather than from the row
    // struct's own metadata.
    std::uintptr_t element_type{};
    std::uintptr_t first_instance{};
    std::uintptr_t first_row{};
    for (const auto& [key, row] : rows) {
        std::uintptr_t instance{};
        if (!reader.Value(row + static_cast<std::uintptr_t>(element_data_offset), instance) ||
            instance == 0) {
            continue;
        }
        if (first_instance == 0) {
            first_instance = instance;
            first_row = row;
        }
        // The slot holds the UScriptStruct pointer directly, confirmed live as
        // name=CharacterItemData class=ScriptStruct with a valid child chain.
        std::uintptr_t child{};
        if (!reader.Value(instance + layout.struct_child_properties, child) ||
            child == 0) {
            continue;
        }
        std::uint32_t child_name_id{};
        std::string child_name;
        if (!reader.Value(child + layout.field_name, child_name_id) ||
            !ResolveName(services.names, child_name_id, child_name) ||
            child_name.empty()) {
            continue;
        }
        element_type = instance;
        break;
    }
    if (element_type == 0) {
        char piece[256]{};
        std::snprintf(piece, sizeof(piece),
            "no readable ElementData type; row=0x%llX instance=0x%llX rows=%zu offset=%d",
            static_cast<unsigned long long>(first_row),
            static_cast<unsigned long long>(first_instance), rows.size(),
            static_cast<int>(element_data_offset));
        error = piece;
        return result;
    }
    if (!FindField(reader, services.names, layout, element_type, kElementFieldName,
            element_field)) {
        std::uint32_t type_name{};
        std::string type_text;
        static_cast<void>(ResolveName(services.names,
            reader.Value(element_type + layout.object_name_offset, type_name)
                ? type_name : 0, type_text));
        error = "CharacterElementType is absent from the ElementData type (type=0x" +
            Hex(element_type) + ", name=" + type_text + "); fields: " +
            DescribeFields(reader, services.names, layout, element_type);
        return result;
    }
    if (element_field.element_size != 1) {
        error = "CharacterElementType is not a one-byte enum";
        return result;
    }

    // Two hops from the row: the slot holds the instance, and the instance's
    // second eight bytes hold the payload pointer.
    std::vector<std::pair<std::string, std::uint8_t>> decoded;
    decoded.reserve(rows.size());
    for (const auto& [key, row] : rows) {
        std::uintptr_t instance{};
        if (!reader.Value(row + static_cast<std::uintptr_t>(element_data_offset), instance) ||
            instance == 0) {
            continue;
        }
        std::uintptr_t payload{};
        // The row slot names the struct type; the payload pointer follows it at
        // the profile's FInstancedStruct data offset.
        if (!reader.Value(row + static_cast<std::uintptr_t>(element_data_offset) +
                layout.instanced_struct_data, payload) || payload == 0) {
            continue;
        }
        std::uint8_t value{};
        if (!reader.Value(payload + static_cast<std::uintptr_t>(element_field.offset),
                value)) {
            continue;
        }
        decoded.emplace_back(key, value);
    }

    std::string unrecognised;
    for (const auto& [key, value] : decoded) {
        const std::string_view name = ElementName(value);
        if (name.empty()) {
            // Not fatal: an unrecognised ordinal means the enum order differs
            // from the hypothesis. Report the concrete value so the mapping can
            // be corrected from one run instead of guessed twice.
            if (unrecognised.empty()) unrecognised = std::to_string(value);
            else if (unrecognised.size() < 64) unrecognised += "," + std::to_string(value);
            continue;
        }
        result.entries_.emplace(key, std::string(name));
    }

    if (result.entries_.empty()) {
        error = "DT_Character published no readable character elements";
    } else if (!unrecognised.empty()) {
        error = "unrecognised CharacterElementType values: " + unrecognised;
    }
    return result;
}
const std::string& CharacterAttributeSource::FindElement(
    const std::string_view character_id) const {
    static const std::string empty;
    const auto found = entries_.find(std::string(character_id));
    return found == entries_.end() ? empty : found->second;
}

const std::string& CharacterAttributeSource::FindElementForPath(
    const std::string_view object_path) const {
    static const std::string empty;
    const std::string id = CharacterIdFromPath(object_path);
    return id.empty() ? empty : FindElement(id);
}

}  // namespace nte::combat_stats
