#pragma once

#include "anomaly/symbol_resolver.hpp"

#include <cstdint>
#include <functional>
#include <string_view>

namespace anomaly {

inline constexpr std::string_view kUe5ObjectFindFeature = "ue5.object-find";
inline constexpr std::string_view kUe5StaticFindObjectSymbol = "ue5.StaticFindObject";
inline constexpr std::string_view kUe5StaticFindObjectAbiValidator =
    "ue5-static-find-object-abi-v1";

using Ue5ObjectLookup = std::function<std::uintptr_t(const wchar_t* full_path)>;

void RegisterUe5ObjectLookupValidator(FeatureLayoutValidatorRegistry& validators);
[[nodiscard]] Ue5ObjectLookup CreateUe5ObjectLookup(
    const BuildProfile& profile,
    const ProfileResolutionSnapshot& resolution,
    const SymbolMemory& memory);

}  // namespace anomaly
