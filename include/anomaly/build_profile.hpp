#pragma once

#include "anomaly/ue5_network_deserializer.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

inline constexpr std::uint32_t kBuildProfileSchemaVersion = 1;
inline constexpr std::size_t kMaximumBuildProfileBytes = 1024U * 1024U;

struct BuildFingerprint {
    std::string game;
    std::string id;
    std::wstring module;
    std::wstring canonical_path_tail;
    std::uint16_t machine{};
    std::uint32_t timestamp{};
    std::uint32_t image_size{};
    std::uint32_t text_virtual_size{};
    std::string text_sha256;
    std::string file_version;
};

enum class ProfileResolveKind : std::uint8_t {
    Direct,
    RipRelative32,
};

struct ProfileResolveRule {
    ProfileResolveKind kind{ProfileResolveKind::Direct};
    std::size_t offset{};
    std::size_t instruction_size{};
    std::ptrdiff_t addend{};
};

struct ProfileSymbol {
    std::string id;
    std::wstring module;
    std::string section;
    std::string pattern;
    ProfileResolveRule resolve;
    std::vector<std::string> validators;
    std::vector<std::string> required_by;
};

// Each direction needs independently verified recorder evidence. A profile
// declaration is inert until the UE5 network-profile activation layer binds it
// to a validated resolver snapshot.
struct ProfileNetworkCaptureBoundary final {
    std::string feature;
    std::string boundary_symbol;
    std::string abi_validator;
    Ue5PacketCaptureStage capture_stage{Ue5PacketCaptureStage::Unknown};
    Ue5PacketProtection capture_protection{Ue5PacketProtection::Unknown};
};

struct ProfileNetworkProtocol final {
    ProfileNetworkCaptureBoundary inbound;
    ProfileNetworkCaptureBoundary outbound;
    Ue5PacketProtocolProfile protocol;
};

struct BuildProfile {
    std::uint32_t schema_version{kBuildProfileSchemaVersion};
    std::string game;
    std::map<std::string, ProfileSymbol, std::less<>> symbols;
    std::map<std::string, std::vector<std::string>, std::less<>> features;
    // Optional features are internal capabilities or diagnostics. Their
    // failure does not degrade the build-level readiness state and they are
    // not presented as peer services in the compatibility UI.
    std::set<std::string, std::less<>> optional_features;
    // Optional schema-v1 feature maps. Profiles without them retain the
    // original symbol-only feature resolution behavior.
    std::map<std::string, std::vector<std::string>, std::less<>> feature_layout_validators;
    std::map<std::string, std::vector<std::string>, std::less<>> feature_dependencies;
    std::map<std::string, std::int64_t, std::less<>> layout;
    std::optional<ProfileNetworkProtocol> network_protocol;
    std::filesystem::path source;
    std::string source_hash;
    std::string source_channel{"bundled"};
    std::int32_t source_priority{};
};

struct ProfileDiagnostic {
    std::filesystem::path source;
    std::string path;
    std::string message;
};

struct BuildProfileParseResult {
    std::optional<BuildProfile> profile;
    std::vector<ProfileDiagnostic> diagnostics;

    [[nodiscard]] bool Ok() const noexcept {
        return profile.has_value() && diagnostics.empty();
    }
};

[[nodiscard]] std::optional<BuildFingerprint> FingerprintPeFile(
    const std::filesystem::path& path,
    std::string game,
    std::string* error = nullptr);
[[nodiscard]] BuildProfileParseResult ParseBuildProfile(
    std::string_view json,
    std::filesystem::path source = {});
[[nodiscard]] BuildProfileParseResult LoadBuildProfile(
    const std::filesystem::path& path);
[[nodiscard]] std::string_view BuildProfileSchemaJson() noexcept;
struct BuildProfileCatalogSnapshot {
    std::vector<BuildProfile> profiles;
    std::vector<ProfileDiagnostic> diagnostics;
};

struct BuildProfileCatalogLayer {
    std::filesystem::path directory;
    std::string channel;
    std::int32_t priority{};
    bool optional{true};
};

class BuildProfileCatalog final {
public:
    [[nodiscard]] BuildProfileCatalogSnapshot Scan(
        const std::filesystem::path& directory) const;
    // Higher-priority layers shadow the active Profile for the same game.
    [[nodiscard]] BuildProfileCatalogSnapshot ScanLayered(
        std::vector<BuildProfileCatalogLayer> layers) const;
};

}  // namespace anomaly
