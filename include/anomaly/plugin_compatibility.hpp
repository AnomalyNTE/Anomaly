#pragma once

#include "anomaly/plugin_manifest.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace anomaly {

struct AvailablePluginVersion {
    std::string id;
    SemanticVersion version;
};

struct AvailableServiceVersion {
    std::string id;
    std::uint32_t version{};
};

struct PluginCompatibilityContext {
    std::uint32_t api_major{};
    std::uint32_t api_minor{};
    // Both values come from the same validated Build Fingerprint snapshot.
    std::optional<std::string> game_id;
    std::optional<std::string> build_id;
    // Must contain unique IDs; invalid snapshots are rejected before evaluation.
    std::vector<AvailablePluginVersion> plugins;
    // Must contain unique Ready/Degraded services exposed by the service view.
    std::vector<AvailableServiceVersion> services;
};

enum class PluginCompatibilityIssueCode : std::uint16_t {
    None = 0x0000,
    ApiMajorMismatch = 0x1001,
    ApiMinorBelowMinimum = 0x1002,
    ApiMinorAboveMaximum = 0x1003,
    CurrentGameUnknown = 0x1101,
    GameMismatch = 0x1102,
    CurrentBuildUnknown = 0x1103,
    GameBuildIdentityMismatch = 0x1104,
    BuildMismatch = 0x1105,
    PluginDependencyMissing = 0x1201,
    PluginDependencyVersionMismatch = 0x1202,
    ServiceMissing = 0x1301,
    ServiceVersionTooLow = 0x1302,
    DuplicateContextPlugin = 0x1f01,
    DuplicateContextService = 0x1f02,
};

struct PluginCompatibilityIssue {
    PluginCompatibilityIssueCode code{PluginCompatibilityIssueCode::None};
    bool blocking{true};
    std::string path;
    std::string subject;
    std::string expected;
    std::optional<std::string> actual;
};

struct PluginCompatibilityResult {
    std::vector<PluginCompatibilityIssue> issues;

    [[nodiscard]] bool Compatible() const noexcept;
    [[nodiscard]] bool Degraded() const noexcept;
};

// Compatibility-relevant fields must satisfy ParsePluginManifest invariants.
[[nodiscard]] PluginCompatibilityResult EvaluatePluginCompatibility(
    const PluginManifest& manifest, const PluginCompatibilityContext& context);

}  // namespace anomaly
