#include "anomaly/ue5_cooked_properties.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace anomaly {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint32_t MaxValues = 1000000;
struct Invalid : std::runtime_error { using std::runtime_error::runtime_error; };
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t pos{};
    void Skip(std::size_t n) {
        if (n > bytes.size() - pos) throw Invalid("cooked value outside export");
        pos += n;
    }
    template<class T> T Get() {
        const auto at = pos; Skip(sizeof(T)); T value{};
        std::memcpy(&value, bytes.data() + at, sizeof(T)); return value;
    }
    std::uint32_t Count() {
        const auto value = Get<std::uint32_t>();
        if (value > MaxValues) throw Invalid("invalid cooked collection count");
        return value;
    }
};
struct Slot { std::uint32_t index{}; bool zero{}; };
struct Header { std::vector<Slot> slots; std::size_t size{}; };
Header ReadHeader(Reader& r) {
    struct Fragment { std::uint32_t start{}, count{}; bool masked{}; };
    std::vector<Fragment> fragments;
    std::uint32_t index{}, mask_count{};
    for (;;) {
        if (fragments.size() >= 65536) throw Invalid("cooked header fragment count exceeds limit");
        const auto word = r.Get<std::uint16_t>();
        index += word & 127u;
        const auto count = std::uint32_t(word >> 9);
        if (index > 65536 || count > 65536 - index) throw Invalid("cooked property index exceeds limit");
        fragments.push_back({index, count, (word & 128u) != 0});
        if (word & 128u) mask_count += count;
        index += count;
        if (word & 256u) break;
    }
    const auto mask_bytes = mask_count == 0 ? 0u : mask_count <= 8 ? 1u : mask_count <= 16 ? 2u : ((mask_count + 31) / 32) * 4;
    const auto mask_at = r.pos; r.Skip(mask_bytes);
    Header result;
    std::uint32_t bit{};
    for (const auto& f : fragments) for (std::uint32_t i{}; i < f.count; ++i) {
        bool zero{};
        if (f.masked) { zero = (r.bytes[mask_at + bit / 8] & (1u << (bit % 8))) != 0; ++bit; }
        result.slots.push_back({f.start + i, zero});
    }
    result.size = r.pos;
    return result;
}
void Put16(Bytes& bytes, std::uint16_t word) {
    bytes.push_back(static_cast<std::uint8_t>(word)); bytes.push_back(static_cast<std::uint8_t>(word >> 8));
}
Bytes WriteHeader(const std::vector<Slot>& slots) {
    Bytes bytes, mask;
    std::uint32_t cursor{}, mask_count{};
    if (slots.empty()) { Put16(bytes, 256); return bytes; }
    for (std::size_t at{}; at < slots.size();) {
        if (slots[at].index < cursor) throw Invalid("non-monotonic cooked property migration");
        auto skip = slots[at].index - cursor;
        while (skip > 127) { Put16(bytes, 127); skip -= 127; }
        const auto begin = at++;
        while (at < slots.size() && at - begin < 127 && slots[at].index == slots[at - 1].index + 1) ++at;
        const bool masked = std::any_of(slots.begin() + begin, slots.begin() + at, [](const auto& s) { return s.zero; });
        if (masked) for (auto i = begin; i < at; ++i) {
            if (mask_count % 8 == 0) mask.push_back(0);
            if (slots[i].zero) mask.back() |= static_cast<std::uint8_t>(1u << (mask_count % 8));
            ++mask_count;
        }
        Put16(bytes, static_cast<std::uint16_t>(skip | (masked ? 128 : 0) | ((at - begin) << 9) | (at == slots.size() ? 256 : 0)));
        cursor = slots[at - 1].index + 1;
    }
    if (mask_count > 16) mask.resize(((mask_count + 31) / 32) * 4);
    bytes.insert(bytes.end(), mask.begin(), mask.end());
    return bytes;
}
std::size_t IntegerSize(std::string_view type) {
    if (type == "ByteProperty" || type == "Int8Property" || type == "BoolProperty") return 1;
    if (type == "Int16Property" || type == "UInt16Property") return 2;
    if (type == "IntProperty" || type == "UInt32Property" || type == "FloatProperty") return 4;
    if (type == "Int64Property" || type == "UInt64Property" || type == "DoubleProperty") return 8;
    return 0;
}
std::size_t RawStructSize(std::string_view name) {
    // UE value serialization contracts, not in-memory game layouts.
    if (name == "Vector" || name == "Rotator") return 24;
    if (name == "Quat" || name == "Vector4") return 32;
    if (name == "Vector2D" || name == "Quat4f" || name == "Vector4f" || name == "LinearColor" || name == "Guid") return 16;
    if (name == "Vector3f" || name == "Rotator3f" || name == "IntVector") return 12;
    if (name == "Vector2f" || name == "IntPoint" || name == "DateTime" || name == "Timespan") return 8;
    if (name == "Color") return 4;
    if (name == "Transform") return 80;
    if (name == "Transform3f") return 40;
    return 0;
}
struct Values {
    const Ue5CookedSchemas& schemas;
    std::size_t names{}, imports{}, exports{};
    std::uint32_t work{};
    void Name(Reader& r) const {
        const auto index = r.Get<std::uint32_t>(), number = r.Get<std::uint32_t>();
        if (index >= names || number > MaxValues) throw Invalid("invalid cooked name reference");
    }
    void Object(Reader& r) const {
        const auto index = r.Get<std::int32_t>();
        if (index > 0 ? std::uint64_t(index) > exports : std::uint64_t(-std::int64_t(index)) > imports)
            throw Invalid("invalid cooked object reference");
    }
    void String(Reader& r) const {
        const auto n = r.Get<std::int32_t>();
        const auto count = n < 0 ? -std::int64_t(n) : std::int64_t(n);
        if (count > MaxValues) throw Invalid("cooked string exceeds limit");
        const auto width = n < 0 ? 2u : 1u;
        r.Skip(static_cast<std::size_t>(count) * width);
        if (count && (r.bytes[r.pos - 1] || (width == 2 && r.bytes[r.pos - 2])))
            throw Invalid("unterminated cooked string");
    }
    void Properties(Reader& r, std::span<const Ue5CookedProperty> properties, unsigned depth) {
        const auto header = ReadHeader(r);
        for (const auto slot : header.slots) {
            if (slot.index >= properties.size()) throw Invalid("cooked property has no declaration");
            if (!slot.zero) Value(r, properties[slot.index].type, depth + 1, true);
        }
    }
    void Value(Reader& r, const Ue5CookedType& type, unsigned depth, bool unversioned = false) {
        if (depth > 32 || ++work > MaxValues) throw Invalid("cooked value traversal exceeds limit");
        const auto& t = type.kind;
        if (t == "BoolProperty") { if (r.Get<std::uint8_t>() > 1) throw Invalid("invalid cooked bool"); }
        else if (const auto width = IntegerSize(t)) r.Skip(width);
        else if (t == "EnumProperty") {
            if (unversioned) {
                if (type.enum_size != 1 && type.enum_size != 2 && type.enum_size != 4 && type.enum_size != 8)
                    throw Invalid("invalid cooked enum width");
                r.Skip(type.enum_size);
            } else Name(r);
        } else if (t == "NameProperty") Name(r);
        else if (t == "ObjectProperty" || t == "WeakObjectProperty" || t == "InterfaceProperty") Object(r);
        else if (t == "LazyObjectProperty") r.Skip(16);
        else if (t == "StrProperty" || t == "Utf8StrProperty" || t == "AnsiStrProperty") String(r);
        else if (t == "SoftObjectProperty" || t == "AssetObjectProperty") { Name(r); Name(r); String(r); }
        else if (t == "DelegateProperty") { Object(r); Name(r); }
        else if (t == "MulticastDelegateProperty") {
            const auto n = r.Count();
            for (std::uint32_t i{}; i < n; ++i) { Object(r); Name(r); }
        } else if (t == "ArrayProperty" || t == "SetProperty") {
            if (type.inner.size() != 1) throw Invalid("invalid cooked collection declaration");
            if (t == "SetProperty") {
                const auto n = r.Count();
                for (std::uint32_t i{}; i < n; ++i) Value(r, type.inner[0], depth + 1);
            }
            const auto n = r.Count();
            for (std::uint32_t i{}; i < n; ++i) Value(r, type.inner[0], depth + 1);
        } else if (t == "MapProperty") {
            if (type.inner.size() != 2) throw Invalid("invalid cooked map declaration");
            const auto removed = r.Count();
            for (std::uint32_t i{}; i < removed; ++i) Value(r, type.inner[0], depth + 1);
            const auto n = r.Count();
            for (std::uint32_t i{}; i < n; ++i) {
                Value(r, type.inner[0], depth + 1); Value(r, type.inner[1], depth + 1);
            }
        } else if (t == "OptionalProperty") {
            if (type.inner.size() != 1) throw Invalid("invalid cooked optional declaration");
            const auto present = r.Get<std::uint32_t>(); // Binary archive optional-field presence uses FArchive's bool.
            if (present > 1) throw Invalid("invalid cooked optional value");
            if (present) Value(r, type.inner[0], depth + 1);
        } else if (t == "StructProperty") {
            if (const auto n = RawStructSize(type.structure)) { r.Skip(n); return; }
            if (type.structure == "PointerToUberGraphFrame") {
                if (!ReadHeader(r).slots.empty()) throw Invalid("unsupported ubergraph frame value");
                return;
            }
            const auto found = schemas.find(type.structure);
            if (found == schemas.end() || found->second.native_serializer)
                throw Invalid("unsupported native cooked struct serializer");
            Properties(r, found->second.properties, depth + 1);
        } else if (t == "TextProperty") {
            static_cast<void>(r.Get<std::uint32_t>());
            const auto history = r.Get<std::int8_t>();
            if (history == 0) { String(r); String(r); String(r); }
            else if (history == 11) { Name(r); String(r); }
            else if (history == -1) {
                const auto source = r.Get<std::uint32_t>();
                if (source > 1) throw Invalid("invalid cooked text source flag");
                if (source) String(r);
            } else throw Invalid("unsupported cooked text history");
        } else throw Invalid("unsupported cooked property type");
    }
};
bool ValidDefault(std::span<const std::uint8_t> bytes, std::span<const Ue5CookedProperty> properties,
    const Ue5CookedSchemas& schemas, std::size_t names, std::size_t imports, std::size_t exports) {
    try {
        Reader r{bytes}; Values{schemas, names, imports, exports}.Properties(r, properties, 0);
        return r.Get<std::uint32_t>() == 0 && r.pos == bytes.size();
    } catch (const Invalid&) { return false; }
}
} // namespace

Ue5CookedDefaultResult MigrateUe5CookedDefault(std::span<const std::uint8_t> bytes,
    std::span<const Ue5CookedProperty> blueprint_properties, const Ue5CookedPropertyMigration& migration,
    std::size_t name_count, std::size_t import_count, std::size_t export_count) {
    if (!migration.current) return {};
    const auto root = migration.current->find(migration.native_class);
    if (root == migration.current->end() || migration.inserted_property >= root->second.properties.size()) return {};
    std::vector<Ue5CookedProperty> current(blueprint_properties.begin(), blueprint_properties.end());
    current.insert(current.end(), root->second.properties.begin(), root->second.properties.end());
    auto legacy = current;
    const auto insertion = blueprint_properties.size() + migration.inserted_property;
    legacy.erase(legacy.begin() + insertion);
    const bool current_valid = ValidDefault(bytes, current, *migration.current, name_count, import_count, export_count);
    const bool legacy_valid = ValidDefault(bytes, legacy, *migration.current, name_count, import_count, export_count);
    if (!legacy_valid) return {current_valid, {}};
    Reader r{bytes}; auto header = ReadHeader(r);
    bool changed{};
    for (auto& slot : header.slots) if (slot.index >= insertion) { ++slot.index; changed = true; }
    if (!changed) return {current_valid, {}};
    if (current_valid) return {}; // Both interpretations fit: keep the existing safe fallback.
    auto converted = WriteHeader(header.slots);
    converted.insert(converted.end(), bytes.begin() + header.size, bytes.end());
    if (!ValidDefault(converted, current, *migration.current, name_count, import_count, export_count)) return {};
    return {true, std::move(converted)};
}

std::uint64_t HashUe5CookedSchemas(const Ue5CookedSchemas& schemas) {
    std::uint64_t hash = 14695981039346656037ull;
    const auto byte = [&](std::uint8_t value) { hash = (hash ^ value) * 1099511628211ull; };
    const auto number = [&](std::uint32_t value) { for (unsigned i{}; i < 4; ++i) byte(static_cast<std::uint8_t>(value >> (i * 8))); };
    const auto string = [&](const std::string& value) { number(static_cast<std::uint32_t>(value.size())); for (auto c : value) byte(static_cast<std::uint8_t>(c)); };
    std::function<void(const Ue5CookedType&)> type;
    type = [&](const Ue5CookedType& value) {
        string(value.kind); string(value.structure); number(value.enum_size); number(static_cast<std::uint32_t>(value.inner.size()));
        for (const auto& inner : value.inner) type(inner);
    };
    number(1); // Canonical schema contract, independent of process/build identity.
    for (const auto& [name, schema] : schemas) {
        string(name); number(static_cast<std::uint32_t>(schema.properties.size()));
        for (const auto& property : schema.properties) { string(property.name); number(property.array_index); type(property.type); }
    }
    return hash;
}

} // namespace anomaly
