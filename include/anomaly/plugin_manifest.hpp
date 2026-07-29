#pragma once

#include "anomaly/semver.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

inline constexpr std::uint32_t kPluginManifestSchemaVersion = 2;
inline constexpr std::uint32_t kLatestPluginManifestSchemaVersion =
    kPluginManifestSchemaVersion;
inline constexpr std::size_t kMaximumPluginManifestBytes = 256U * 1024U;
inline constexpr std::size_t kMaximumPluginManifestNesting = 64;
inline constexpr std::size_t kMaximumPluginManifestNodes = 4096;
inline constexpr std::size_t kMaximumPluginManifestDiagnostics = 128;
inline constexpr std::size_t kUnknownManifestOffset =
    (std::numeric_limits<std::size_t>::max)();

enum class PluginLoadPhase : std::uint8_t {
    Unknown = 0,
    GameReady = 1,
};

enum class PluginAudience : std::uint8_t {
    User = 0,
    Developer = 1,
};

struct PluginApiCompatibility {
    std::uint32_t major{};
    std::uint32_t minimum_minor{};
    std::uint32_t maximum_minor{};
};

struct PluginDependencyManifest {
    std::string id;
    std::string version_expression;
    SemanticVersionRange version_range;
    bool optional{};
};

struct PluginServiceRequirement {
    std::string id;
    std::uint32_t minimum_version{1};
    bool optional{};
};

struct PluginManifest {
    std::uint32_t schema_version{kPluginManifestSchemaVersion};
    std::string id;
    std::string name;
    std::string description;
    std::string author;
    std::string license;
    PluginAudience audience{PluginAudience::User};
    SemanticVersion version;
    std::string entry;
    PluginApiCompatibility api;
    std::vector<std::string> games;
    std::vector<std::string> builds;
    PluginLoadPhase load_phase{PluginLoadPhase::Unknown};
    std::vector<PluginDependencyManifest> dependencies;
    std::vector<PluginServiceRequirement> services;
    std::vector<std::string> capabilities;
};

enum class PluginManifestErrorCode : std::uint16_t {
    None = 0x0000,
    DocumentTooLarge = 0x0101,
    InvalidUtf8 = 0x0102,
    EmbeddedNull = 0x0103,
    JsonSyntax = 0x0104,
    DuplicateJsonKey = 0x0105,
    RootNotObject = 0x0106,
    DocumentTooDeep = 0x0107,
    DocumentTooComplex = 0x0108,
    UnsupportedSchemaVersion = 0x0201,
    SchemaViolation = 0x0202,
    InvalidSemanticVersion = 0x0301,
    InvalidVersionRange = 0x0302,
    InvalidApiRange = 0x0303,
    SelfDependency = 0x0304,
    UnownedBuildPattern = 0x0305,
    GameWithoutBuildPattern = 0x0306,
    OverlappingGameId = 0x0307,
    DuplicateDependency = 0x0310,
    DuplicateService = 0x0311,
    InternalFailure = 0x7fff,
};

struct PluginManifestDiagnostic {
    PluginManifestErrorCode code{PluginManifestErrorCode::None};
    std::string path;
    std::size_t source_offset{kUnknownManifestOffset};
    std::size_t value_offset{kUnknownManifestOffset};
    std::string message;
};

struct PluginManifestParseResult {
    std::optional<PluginManifest> manifest;
    std::vector<PluginManifestDiagnostic> diagnostics;

    [[nodiscard]] bool Ok() const noexcept {
        return manifest.has_value() && diagnostics.empty();
    }
};

[[nodiscard]] PluginManifestParseResult ParsePluginManifest(std::string_view utf8_json);
[[nodiscard]] std::string_view PluginManifestSchemaJson() noexcept;

}  // namespace anomaly
