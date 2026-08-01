#include "anomaly/plugin_capability_policy.hpp"

#include <array>
#include <iostream>
#include <string>
#include <string_view>

namespace {

bool Check(bool value, std::string_view message) {
    if (value) return true;
    std::cerr << message << '\n';
    return false;
}

anomaly::PluginManifest Manifest() {
    anomaly::PluginManifest result;
    result.schema_version = anomaly::kPluginManifestSchemaVersion;
    return result;
}

bool TestPlayerTeleportRequiresExplicitManifestGrant() {
    auto manifest = Manifest();
    manifest.services = {{"anomaly.nte.player-teleport", 1, false}};
    auto inferred = anomaly::ResolvePluginCapabilityGrant(&manifest);
    if (!Check(!inferred.IsEnforceable() &&
                   !inferred.AuthorizeService("anomaly.nte.player-teleport").allowed &&
                   inferred.Audits().size() == 1 &&
                   inferred.Audits().front().code ==
                       anomaly::PluginCapabilityAuditCode::RequiredServiceMissingCapability,
               "player teleport requirement inferred a mutation capability")) {
        return false;
    }

    manifest.capabilities = {"nte-player-teleport"};
    const auto explicit_grant = anomaly::ResolvePluginCapabilityGrant(&manifest);
    const auto teleport = explicit_grant.AuthorizeService("anomaly.nte.player-teleport");
    return Check(explicit_grant.IsEnforceable() && teleport.allowed &&
                     teleport.required_capability == "nte-player-teleport",
                 "explicit player-teleport capability did not authorize the mutation service");
}

bool TestExplicitCapabilitiesAndUnknownAudit() {
    auto manifest = Manifest();
    manifest.capabilities = {"ui", "nte-player-snapshot", "ue5-names", "unknown.fixture"};
    const auto grant = anomaly::ResolvePluginCapabilityGrant(&manifest);
    return Check(grant.AuthorizeService("anomaly.ui").allowed,
                 "declared ui capability did not grant the UI service") &&
        Check(grant.AuthorizeService("anomaly.nte.player").allowed,
              "declared NTE player capability did not grant its service") &&
        Check(grant.AuthorizeService("anomaly.ue5.names").allowed,
              "declared UE5 name capability did not grant its service") &&
        Check(!grant.AuthorizeService("anomaly.nte.entities").allowed,
              "undeclared NTE entity service was granted") &&
        Check(grant.Audits().size() == 1 &&
                  grant.Audits().front().code ==
                      anomaly::PluginCapabilityAuditCode::UnknownCapability &&
                  grant.Audits().front().capability == "unknown.fixture",
              "unknown capability did not retain its diagnostic");
}

bool TestUnknownServiceDefaultsToDeny() {
    const auto manifest = Manifest();
    const auto grant = anomaly::ResolvePluginCapabilityGrant(&manifest);
    return Check(!grant.AuthorizeService("anomaly.future.service").allowed,
                 "unmapped service was exposed to a manifest-backed plugin");
}

bool TestEveryControlledServiceRequiresItsMappedCapability() {
    struct Mapping final {
        std::string_view service;
        std::string_view capability;
    };
    constexpr std::array mappings{
        Mapping{"anomaly.plugin-state", "configuration"},
        Mapping{"anomaly.config", "configuration"},
        Mapping{"anomaly.storage", "storage"},
        Mapping{"anomaly.runtime-info", "runtime-info"},
        Mapping{"anomaly.diagnostics", "diagnostics"},
        Mapping{"anomaly.scheduler", "scheduler"},
        Mapping{"anomaly.ipc", "ipc"},
        Mapping{"anomaly.commands", "commands"},
        Mapping{"anomaly.notifications", "notifications"},
        Mapping{"anomaly.interop.signature", "interop-signature"},
        Mapping{"anomaly.interop.hook", "interop-hook"},
        Mapping{"anomaly.interop.patch", "interop-patch"},
        Mapping{"anomaly.ui", "ui"},
        Mapping{"anomaly.window", "ui-window"},
        Mapping{"anomaly.font", "ui-font"},
        Mapping{"anomaly.texture", "ui-texture"},
        Mapping{"anomaly.input", "input"},
        Mapping{"anomaly.ue5.build", "ue5-build"},
        Mapping{"anomaly.ue5.ahud", "ue5-ahud"},
        Mapping{"anomaly.ue5.framework", "game-events"},
        Mapping{"anomaly.ue5.names", "ue5-names"},
        Mapping{"anomaly.ue5.objects", "ue5-objects"},
        Mapping{"anomaly.ue5.world", "ue5-world"},
        Mapping{"anomaly.nte.build", "nte-build"},
        Mapping{"anomaly.nte.session", "nte-session-snapshot"},
        Mapping{"anomaly.nte.metrics", "nte-snapshot-metrics"},
        Mapping{"anomaly.nte.player", "nte-player-snapshot"},
        Mapping{"anomaly.nte.player-teleport", "nte-player-teleport"},
        Mapping{"anomaly.nte.entities", "nte-entity-snapshot"},
        Mapping{"anomaly.nte.esc-menu-button", "nte-esc-menu-button"},
        Mapping{"anomaly.nte.actors", "nte-actor-snapshot"},
    };
    for (const Mapping& mapping : mappings) {
        auto manifest = Manifest();
        manifest.capabilities = {std::string(mapping.capability)};
        const auto allowed = anomaly::ResolvePluginCapabilityGrant(&manifest).AuthorizeService(mapping.service);
        if (!Check(allowed.allowed && allowed.required_capability == mapping.capability,
                   "declared capability did not authorize its mapped service")) {
            return false;
        }
        manifest.capabilities.clear();
        const auto denied = anomaly::ResolvePluginCapabilityGrant(&manifest).AuthorizeService(mapping.service);
        if (!Check(!denied.allowed && denied.required_capability == mapping.capability,
                   "missing capability did not deny its mapped service")) {
            return false;
        }
    }
    return true;
}

bool TestRawMemoryCapabilityBoundary() {
    auto manifest = Manifest();
    auto grant = anomaly::ResolvePluginCapabilityGrant(&manifest);
    if (!Check(!grant.AuthorizeRawMemory("memory-read").allowed &&
                   !grant.AuthorizeRawMemory("memory-write").allowed &&
                   grant.EnforcesRawMemoryCapabilities(),
               "package received an undeclared raw-memory grant")) {
        return false;
    }

    manifest.capabilities = {"memory-read"};
    grant = anomaly::ResolvePluginCapabilityGrant(&manifest);
    if (!Check(grant.AuthorizeRawMemory("memory-read").allowed &&
                   !grant.AuthorizeRawMemory("memory-write").allowed,
               "memory-read capability did not remain read-only")) {
        return false;
    }

    manifest.capabilities = {"memory-write"};
    grant = anomaly::ResolvePluginCapabilityGrant(&manifest);
    if (!Check(!grant.AuthorizeRawMemory("memory-read").allowed &&
                   grant.AuthorizeRawMemory("memory-write").allowed,
               "memory-write capability did not remain write-only")) {
        return false;
    }

    char caller_owned_capability[] = "memory-read";
    const auto caller_owned_authorization = grant.AuthorizeRawMemory(caller_owned_capability);
    caller_owned_capability[0] = 'x';
    if (!Check(caller_owned_authorization.required_capability == "memory-read",
               "raw-memory authorization retained caller-owned storage")) {
        return false;
    }

    manifest.schema_version = anomaly::kLatestPluginManifestSchemaVersion + 1;
    manifest.capabilities.clear();
    const auto unknown = anomaly::ResolvePluginCapabilityGrant(&manifest);
    return Check(!unknown.AuthorizeRawMemory("memory-read").allowed &&
                     !unknown.AuthorizeRawMemory("memory-write").allowed &&
                     unknown.EnforcesRawMemoryCapabilities(),
                 "unknown manifest schema bypassed raw-memory capability enforcement");
}

bool TestManifestEnforcement() {
    auto manifest = Manifest();
    manifest.services = {{"anomaly.scheduler", 1, false}};
    auto grant = anomaly::ResolvePluginCapabilityGrant(&manifest);
    if (!Check(!grant.IsEnforceable(),
               "required service without its capability was accepted") ||
        !Check(grant.Audits().size() == 1 &&
                   grant.Audits().front().code ==
                       anomaly::PluginCapabilityAuditCode::RequiredServiceMissingCapability,
               "missing capability did not have a typed audit")) {
        return false;
    }

    manifest.capabilities = {"scheduler"};
    grant = anomaly::ResolvePluginCapabilityGrant(&manifest);
    if (!Check(grant.IsEnforceable() &&
                   grant.AuthorizeService("anomaly.scheduler").allowed,
               "declared capability did not grant its required service")) {
        return false;
    }

    manifest.capabilities = {"scheduler", "unknown.fixture"};
    grant = anomaly::ResolvePluginCapabilityGrant(&manifest);
    return Check(!grant.IsEnforceable(),
                 "unknown capability was not rejected before activation") &&
        Check(grant.Audits().size() == 1 &&
                  grant.Audits().front().code ==
                      anomaly::PluginCapabilityAuditCode::UnknownCapability,
              "unknown capability did not retain its diagnostic");
}

}  // namespace

int main() {
    return TestPlayerTeleportRequiresExplicitManifestGrant() &&
            TestExplicitCapabilitiesAndUnknownAudit() && TestUnknownServiceDefaultsToDeny() &&
            TestEveryControlledServiceRequiresItsMappedCapability() &&
            TestRawMemoryCapabilityBoundary() && TestManifestEnforcement()
        ? 0
        : 1;
}
