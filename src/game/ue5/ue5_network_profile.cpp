#include "anomaly/ue5_network_profile.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace anomaly {
namespace {

[[nodiscard]] bool FeatureRequiresSymbol(
    const BuildProfile& profile,
    const std::string_view feature,
    const std::string_view symbol) {
    const auto found = profile.features.find(std::string(feature));
    return found != profile.features.end() &&
        std::find(found->second.begin(), found->second.end(), std::string(symbol)) !=
            found->second.end();
}

[[nodiscard]] bool FeatureRequiresValidator(
    const BuildProfile& profile,
    const std::string_view feature,
    const std::string_view validator) {
    const auto found = profile.feature_layout_validators.find(std::string(feature));
    return found != profile.feature_layout_validators.end() &&
        std::find(found->second.begin(), found->second.end(), std::string(validator)) !=
            found->second.end();
}

[[nodiscard]] bool ActivateBoundary(
    const BuildProfile& profile,
    const ProfileResolutionSnapshot& resolution,
    const ProfileNetworkCaptureBoundary& declaration,
    const Ue5PacketDirection direction,
    const std::string_view name,
    ActivatedUe5PacketCaptureBoundary& activated,
    std::string& diagnostic) {
    if (declaration.capture_stage != Ue5PacketCaptureStage::PostPacketHandler ||
        declaration.capture_protection != Ue5PacketProtection::Clear) {
        diagnostic = std::string(name) +
            " capture boundary is not a validated clear post-handler boundary";
        return false;
    }
    if (!FeatureRequiresSymbol(profile, declaration.feature, declaration.boundary_symbol)) {
        diagnostic = std::string(name) +
            " capture boundary is not required by its declared feature";
        return false;
    }
    if (!FeatureRequiresValidator(profile, declaration.feature, declaration.abi_validator)) {
        diagnostic = std::string(name) +
            " capture ABI validator is not required by its declared feature";
        return false;
    }
    if (!resolution.FeatureAvailable(declaration.feature)) {
        diagnostic = std::string(name) + " capture feature is unavailable in the resolver snapshot";
        return false;
    }
    const ResolvedSymbol* symbol = resolution.FindSymbol(declaration.boundary_symbol);
    if (symbol == nullptr || !symbol->Available() || symbol->address == 0U) {
        diagnostic = std::string(name) +
            " capture boundary is unavailable in the resolver snapshot";
        return false;
    }

    activated.direction = direction;
    activated.feature = declaration.feature;
    activated.abi_validator = declaration.abi_validator;
    activated.symbol = declaration.boundary_symbol;
    activated.address = symbol->address;
    activated.stage = declaration.capture_stage;
    activated.protection = declaration.capture_protection;
    return true;
}

}  // namespace

std::optional<ActivatedUe5PacketProtocol> ActivateUe5PacketProtocol(
    const BuildProfile& profile,
    const ProfileResolutionSnapshot& resolution,
    std::string* error) {
    const auto fail = [&](std::string message) -> std::optional<ActivatedUe5PacketProtocol> {
        if (error != nullptr) *error = std::move(message);
        return std::nullopt;
    };

    if (!profile.network_protocol) {
        return fail("the selected profile does not declare a network protocol");
    }
    if (profile.source_hash.size() != 64U ||
        resolution.profile_hash != profile.source_hash) {
        return fail("the resolver snapshot does not match the selected profile recipe");
    }

    ActivatedUe5PacketProtocol activated;
    std::string diagnostic;
    const ProfileNetworkProtocol& declaration = *profile.network_protocol;
    if (!ActivateBoundary(
            profile, resolution, declaration.inbound, Ue5PacketDirection::Inbound, "inbound",
            activated.inbound, diagnostic) ||
        !ActivateBoundary(
            profile, resolution, declaration.outbound, Ue5PacketDirection::Outbound, "outbound",
            activated.outbound, diagnostic)) {
        return fail(std::move(diagnostic));
    }

    activated.protocol = declaration.protocol;
    activated.protocol.profile_hash = profile.source_hash;
    activated.protocol.post_handler_boundary_validated = true;
    if (const auto validation_error = ValidateUe5PacketProtocolProfile(activated.protocol)) {
        return fail("the packet protocol is invalid: " + *validation_error);
    }

    if (error != nullptr) error->clear();
    return activated;
}

}  // namespace anomaly
