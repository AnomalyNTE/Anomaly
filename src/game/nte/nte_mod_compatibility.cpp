#include "anomaly/nte_mod_compatibility.hpp"
#include "anomaly/ue5_assets.hpp"

#include <limits>
#include <stdexcept>

namespace anomaly {
std::shared_ptr<const Ue5CookedPropertyMigration> CreateNteVehiclePropertyMigration(
    const BuildProfile& profile, const ProfileResolutionSnapshot& resolution,
    const SymbolMemory& memory, const Ue5ObjectLookup& lookup) {
    auto migration = std::make_shared<Ue5CookedPropertyMigration>();
    migration->native_class = "HTWheeledVehicleDrivable";
    migration->native_path = "/Script/HTGame/HTWheeledVehicleDrivable";
    const auto value = [&](std::string_view key) -> std::uint32_t {
        const auto found = profile.layout.find(key);
        if (found == profile.layout.end() || found->second < 0 || found->second > (std::numeric_limits<std::uint32_t>::max)())
            throw std::runtime_error("vehicle cooked contract unavailable: " + std::string(key));
        return static_cast<std::uint32_t>(found->second);
    };
    if (!profile.layout.contains("cooked.vehicle.schemaHashLo")) return migration;
    const auto* names = resolution.FindSymbol("ue5.FNamePool");
    if (!names || !names->Available() || !lookup) return migration;
    const auto root = lookup(L"/Script/HTGame.HTWheeledVehicleDrivable");
    if (!root) throw std::runtime_error("native vehicle metadata is not ready");
    try {
        const Ue5AssetNamePool pool{names->address + value("names.blocksOffset"), value("names.blockBits"),
            value("names.entryStride"), value("names.headerLengthShift")};
        auto schemas = std::make_shared<Ue5CookedSchemas>(SnapshotUe5CookedSchemas(profile, memory, root,
            [&](std::uint32_t id) { return ReadUe5AssetName(pool, memory, {id, 0}); }));
        const auto expected = std::uint64_t(value("cooked.vehicle.schemaHashHi")) << 32 | value("cooked.vehicle.schemaHashLo");
        const auto hash = HashUe5CookedSchemas(*schemas);
        const auto insertion = value("cooked.vehicle.insertedProperty");
        const auto current = schemas->find(migration->native_class);
        if (hash != expected || current == schemas->end() || current->second.properties.size() != value("cooked.vehicle.currentPropertyCount") ||
            insertion >= current->second.properties.size() || current->second.properties[insertion].name != "bNeedTickDashboardUI" ||
            current->second.properties[insertion].type.kind != "BoolProperty") return migration;
        migration->inserted_property = insertion;
        migration->current = std::move(schemas);
        migration->schema_hash = hash;
    } catch (const std::runtime_error&) {}
    return migration;
}
} // namespace anomaly
