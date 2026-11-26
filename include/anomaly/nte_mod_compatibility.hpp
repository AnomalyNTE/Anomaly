#pragma once

#include "anomaly/ue5_cooked_properties.hpp"
#include "anomaly/ue5_object_lookup.hpp"

namespace anomaly {

// Worker initialization only. lookup marshals the native class lookup to Game.
// An unavailable contract keeps the affected native root but no graph, so its
// cooked children still take the package-level fallback.
std::shared_ptr<const Ue5CookedPropertyMigration> CreateNteVehiclePropertyMigration(
    const BuildProfile& profile, const ProfileResolutionSnapshot& resolution,
    const SymbolMemory& memory, const Ue5ObjectLookup& lookup);

} // namespace anomaly
