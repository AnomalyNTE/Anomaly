#pragma once

#include "anomaly/build_profile.hpp"
#include "anomaly/symbol_resolver.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace anomaly {

// A resolved boundary is valid only for the BuildProfile and resolver snapshot
// supplied to ActivateUe5PacketProtocol.
struct ActivatedUe5PacketCaptureBoundary final {
    Ue5PacketDirection direction{Ue5PacketDirection::Unknown};
    std::string feature;
    std::string abi_validator;
    std::string symbol;
    std::uintptr_t address{};
    Ue5PacketCaptureStage stage{Ue5PacketCaptureStage::Unknown};
    Ue5PacketProtection protection{Ue5PacketProtection::Unknown};
};

struct ActivatedUe5PacketProtocol final {
    Ue5PacketProtocolProfile protocol;
    ActivatedUe5PacketCaptureBoundary inbound;
    ActivatedUe5PacketCaptureBoundary outbound;
};

// Activates a packet decoder declaration only when both directions are backed
// by the same validated resolver snapshot. Parsing a profile never enables
// packet decoding on its own.
[[nodiscard]] std::optional<ActivatedUe5PacketProtocol> ActivateUe5PacketProtocol(
    const BuildProfile& profile,
    const ProfileResolutionSnapshot& resolution,
    std::string* error = nullptr);

}  // namespace anomaly
