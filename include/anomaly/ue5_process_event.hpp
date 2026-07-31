#pragma once

#include "anomaly/symbol_resolver.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace anomaly {

inline constexpr std::string_view kUe5ProcessEventFeature = "ue5.process-event";
inline constexpr std::string_view kUe5ProcessEventSymbol = "ue5.ProcessEvent";
inline constexpr std::string_view kUe5ProcessEventAbiValidator =
    "ue5-process-event-abi-v1";

using Ue5ProcessEventInvoker = std::function<bool(
    std::uintptr_t object,
    std::uintptr_t function,
    void* parameters,
    std::size_t parameter_size)>;

[[nodiscard]] FeatureLayoutValidatorRegistry Ue5FeatureLayoutValidators();
[[nodiscard]] Ue5ProcessEventInvoker CreateUe5ProcessEventInvoker(
    const BuildProfile& profile,
    const ProfileResolutionSnapshot& resolution,
    const SymbolMemory& memory);

}  // namespace anomaly
