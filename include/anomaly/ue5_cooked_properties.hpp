#pragma once

#include "anomaly/symbol_resolver.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace anomaly {

struct Ue5CookedType {
    std::string kind;
    std::string structure;
    std::uint32_t enum_size{};
    std::vector<Ue5CookedType> inner;
    bool operator==(const Ue5CookedType&) const = default;
};

struct Ue5CookedProperty {
    std::string name;
    std::uint32_t array_index{};
    Ue5CookedType type;
    bool operator==(const Ue5CookedProperty&) const = default;
};

struct Ue5CookedStruct {
    bool native_serializer{};
    std::vector<Ue5CookedProperty> properties;
};
using Ue5CookedSchemas = std::map<std::string, Ue5CookedStruct, std::less<>>;

// An insertion-only native contract, selected by reflected schema rather than
// build identity. The current native graph is an owned, immutable snapshot;
// a null graph marks affected defaults for the package-level fallback.
struct Ue5CookedPropertyMigration {
    std::string native_class;
    std::uint32_t inserted_property{};
    std::shared_ptr<const Ue5CookedSchemas> current;
    std::uint64_t schema_hash{};
    std::string native_path;
};

// Native reflected metadata is immutable after registration. The caller looks
// up the native root on Game; this copies its metadata on Worker without retaining
// any engine pointers. Every memory layout comes from the active Profile.
Ue5CookedSchemas SnapshotUe5CookedSchemas(const BuildProfile& profile,
    const SymbolMemory& memory, std::uintptr_t root,
    const std::function<std::string(std::uint32_t)>& resolve_name);
std::uint64_t HashUe5CookedSchemas(const Ue5CookedSchemas& schemas);

struct Ue5CookedDefaultResult {
    bool compatible{};
    std::vector<std::uint8_t> converted; // Empty when already current/unaffected.
};

// Checks the complete CDO property stream and null sparse-class-data reference
// under both contracts. Ambiguous or unsupported streams remain unconverted.
// Values and bytecode are never regenerated; only the ordinal header can change.
Ue5CookedDefaultResult MigrateUe5CookedDefault(std::span<const std::uint8_t> bytes,
    std::span<const Ue5CookedProperty> blueprint_properties,
    const Ue5CookedPropertyMigration& migration,
    std::size_t name_count, std::size_t import_count, std::size_t export_count);

} // namespace anomaly
