#include "anomaly/ue5_cooked_properties.hpp"

#include <set>
#include <stdexcept>
#include <utility>

namespace anomaly {
namespace {
struct Snapshot {
    const BuildProfile& profile;
    const SymbolMemory& memory;
    const std::function<std::string(std::uint32_t)>& resolve;
    Ue5CookedSchemas schemas;
    std::map<std::uint32_t, std::string> names;
    std::map<std::string, std::uintptr_t, std::less<>> structures;
    std::size_t fields{};

    std::uintptr_t Offset(std::string_view key) const {
        const auto found = profile.layout.find(key);
        if (found == profile.layout.end() || found->second < 0 || found->second > 65536)
            throw std::runtime_error("cooked schema layout unavailable: " + std::string(key));
        return static_cast<std::uintptr_t>(found->second);
    }
    template<class T> T Read(std::uintptr_t address, std::string_view key) const {
        T value{};
        if (!address || !memory.Read(address + Offset(key), &value, sizeof(value)))
            throw std::runtime_error("cooked schema metadata unreadable: " + std::string(key));
        return value;
    }
    std::string Name(std::uintptr_t address, std::string_view key) {
        const auto value = Read<std::uint64_t>(address, key);
        if (value >> 32) throw std::runtime_error("numbered native cooked schema name");
        const auto id = static_cast<std::uint32_t>(value);
        auto [it, inserted] = names.try_emplace(id);
        if (inserted) it->second = resolve(id);
        if (it->second.empty()) throw std::runtime_error("cooked schema name unavailable");
        return it->second;
    }
    std::string NativeName(std::uintptr_t address) {
        const auto outer = Read<std::uintptr_t>(address, "object.outer");
        if (!Name(outer, "object.nameOffset").starts_with("/Script/"))
            throw std::runtime_error("cooked schema requires registered native metadata");
        return Name(address, "object.nameOffset");
    }
    Ue5CookedType Type(std::uintptr_t field, unsigned depth) {
        if (depth > 32 || ++fields > 65536) throw std::runtime_error("cooked schema traversal exceeds limit");
        Ue5CookedType result;
        result.kind = Name(Read<std::uintptr_t>(field, "ffield.class"), "ffieldClass.name");
        auto& kind = result.kind;
        if (kind == "ClassProperty") kind = "ObjectProperty";
        else if (kind == "SoftClassProperty") kind = "SoftObjectProperty";
        else if (kind == "MulticastInlineDelegateProperty" || kind == "MulticastSparseDelegateProperty") kind = "MulticastDelegateProperty";
        else if (kind == "ByteProperty" && Read<std::uintptr_t>(field, "fbyteProperty.enum")) kind = "EnumProperty";
        if (kind == "EnumProperty") {
            const auto size = Read<std::int32_t>(field, "fproperty.elementSize");
            if (size != 1 && size != 2 && size != 4 && size != 8) throw std::runtime_error("invalid cooked enum width");
            result.enum_size = static_cast<std::uint32_t>(size);
        } else if (kind == "StructProperty") {
            const auto structure = Read<std::uintptr_t>(field, "fstructProperty.struct");
            result.structure = Struct(structure, false, depth + 1);
        } else if (kind == "ArrayProperty" || kind == "SetProperty" || kind == "OptionalProperty") {
            const auto key = kind == "ArrayProperty" ? "farrayProperty.inner" :
                kind == "SetProperty" ? "fsetProperty.elementProp" : "foptionalProperty.valueProperty";
            result.inner.push_back(Type(Read<std::uintptr_t>(field, key), depth + 1));
        } else if (kind == "MapProperty") {
            result.inner.push_back(Type(Read<std::uintptr_t>(field, "fmapProperty.keyProp"), depth + 1));
            result.inner.push_back(Type(Read<std::uintptr_t>(field, "fmapProperty.valueProp"), depth + 1));
        }
        return result;
    }
    std::string Struct(std::uintptr_t address, bool root, unsigned depth) {
        if (depth > 32) throw std::runtime_error("cooked schema nesting exceeds limit");
        auto name = NativeName(address);
        const auto [existing, added] = structures.emplace(name, address);
        if (!added) {
            if (existing->second != address) throw std::runtime_error("ambiguous native cooked struct name");
            return name;
        }
        auto& schema = schemas[name];
        if (!root) schema.native_serializer = (Read<std::uint32_t>(address, "uscriptStruct.flags") & 0x800u) != 0;
        std::set<std::uintptr_t> seen;
        for (auto field = Read<std::uintptr_t>(address, "ustruct.propertyLink"); field;
            field = Read<std::uintptr_t>(field, "fproperty.propertyLinkNext")) {
            if (!seen.insert(field).second) throw std::runtime_error("cyclic cooked property link");
            if (seen.size() > 65536) throw std::runtime_error("cooked property link exceeds limit");
            const auto flags = Read<std::uint64_t>(field, "fproperty.flags");
            if (flags & 0x0000000800000000ull) continue; // CPF_EditorOnly is excluded from cooked game schemas.
            const auto dim = Read<std::int32_t>(field, "fproperty.arrayDim");
            if (dim <= 0 || dim > 65536 || static_cast<std::size_t>(dim) > 65536 - schema.properties.size())
                throw std::runtime_error("invalid cooked property array dimension");
            const auto field_name = Name(field, "ffield.name");
            const auto type = Type(field, depth + 1);
            for (std::int32_t i{}; i < dim; ++i) schema.properties.push_back({field_name, static_cast<std::uint32_t>(i), type});
        }
        return name;
    }
};
}

Ue5CookedSchemas SnapshotUe5CookedSchemas(const BuildProfile& profile, const SymbolMemory& memory,
    std::uintptr_t root, const std::function<std::string(std::uint32_t)>& resolve_name) {
    Snapshot snapshot{profile, memory, resolve_name};
    snapshot.Struct(root, true, 0);
    return std::move(snapshot.schemas);
}
} // namespace anomaly
