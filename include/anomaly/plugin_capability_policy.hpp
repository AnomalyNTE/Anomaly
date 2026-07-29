#pragma once

#include "anomaly/plugin_manifest.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

enum class PluginCapabilityAuditCode : std::uint8_t {
    UnknownCapability,
    InferredFromRequiredService,
    RequiredServiceMissingCapability,
    RequiredServiceMissingMapping,
};

struct PluginCapabilityAudit {
    PluginCapabilityAuditCode code{PluginCapabilityAuditCode::UnknownCapability};
    std::string capability;
    std::string service;
};

struct PluginServiceAuthorization {
    bool allowed{};
    std::string_view required_capability;
};

// The host owns the resulting grant; native plugins never supply an identity
// to service queries.
class PluginCapabilityGrant final {
public:
    [[nodiscard]] bool IsEnforceable() const noexcept { return enforceable_; }
    [[nodiscard]] bool EnforcesRawMemoryCapabilities() const noexcept { return true; }
    [[nodiscard]] bool HasCapability(std::string_view capability) const noexcept;
    [[nodiscard]] const std::vector<std::string>& Capabilities() const noexcept {
        return capabilities_;
    }
    [[nodiscard]] PluginServiceAuthorization AuthorizeService(
        std::string_view service_id) const noexcept;
    [[nodiscard]] PluginServiceAuthorization AuthorizeRawMemory(
        std::string_view capability) const noexcept;
    [[nodiscard]] const std::vector<PluginCapabilityAudit>& Audits() const noexcept {
        return audits_;
    }

private:
    friend PluginCapabilityGrant ResolvePluginCapabilityGrant(const PluginManifest* manifest);

    bool enforceable_{true};
    std::vector<std::string> capabilities_;
    std::vector<PluginCapabilityAudit> audits_;
};

[[nodiscard]] PluginCapabilityGrant ResolvePluginCapabilityGrant(
    const PluginManifest* manifest);

}  // namespace anomaly
