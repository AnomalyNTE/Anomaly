#include "anomaly/plugin_manifest.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

std::string ValidManifest() {
    return R"JSON({
  "schemaVersion": 2,
  "id": "com.example.my-plugin",
  "name": "My Plugin",
  "description": "Explains what My Plugin does.",
  "version": "1.2.0-rc.1+build.7",
  "author": "Example",
  "license": "AGPL-3.0-only",
  "audience": "developer",
  "entry": "plugin.dll",
  "api": { "major": 1, "minMinor": 0, "maxMinor": 2 },
  "games": ["nte"],
  "builds": ["nte-win64-*"],
  "loadPhase": "game-ready",
  "dependencies": [
    { "id": "com.example.base", "version": ">=1.0.0 <2.0.0", "optional": false }
  ],
  "services": [
    { "id": "anomaly.ui", "minVersion": 1 },
    { "id": "anomaly.nte.world", "minVersion": 1, "optional": true }
  ],
  "capabilities": ["ui", "game-events", "memory-read"]
})JSON";
}

std::string MinimalManifest() {
    return R"JSON({
  "schemaVersion": 2,
  "id": "com.example.minimal",
  "name": "Minimal",
  "version": "0.1.0",
  "entry": "plugin.dll",
  "api": { "major": 1, "minMinor": 0, "maxMinor": 0 },
  "games": ["nte"],
  "builds": ["nte-win64-20260716"],
  "loadPhase": "game-ready"
})JSON";
}

std::string ReplaceOnce(
    std::string source, std::string_view expected, std::string_view replacement) {
    const std::size_t offset = source.find(expected);
    if (offset == std::string::npos) {
        throw std::runtime_error("manifest test replacement did not match its fixture");
    }
    source.replace(offset, expected.size(), replacement);
    return source;
}

const anomaly::PluginManifestDiagnostic* FindDiagnostic(
    const anomaly::PluginManifestParseResult& result,
    anomaly::PluginManifestErrorCode code, std::string_view path = {}) {
    const auto found = std::find_if(
        result.diagnostics.begin(), result.diagnostics.end(),
        [&](const anomaly::PluginManifestDiagnostic& diagnostic) {
            return diagnostic.code == code &&
                (path.empty() || diagnostic.path == path);
        });
    return found == result.diagnostics.end() ? nullptr : &*found;
}

bool TestValidManifest() {
    const auto result = anomaly::ParsePluginManifest(ValidManifest());
    bool passed = Check(result.Ok(), "Complete plugin manifest did not parse");
    if (!result.manifest) return false;

    const auto& manifest = *result.manifest;
    passed = Check(manifest.schema_version == anomaly::kPluginManifestSchemaVersion &&
                       manifest.id == "com.example.my-plugin" &&
                       manifest.name == "My Plugin" &&
                       manifest.description == "Explains what My Plugin does." &&
                       manifest.author == "Example" &&
                       manifest.license == "AGPL-3.0-only" &&
                       manifest.audience == anomaly::PluginAudience::Developer,
                   "Manifest identity fields were not materialized") && passed;
    passed = Check(manifest.version.ToString() == "1.2.0-rc.1+build.7" &&
                       manifest.entry == "plugin.dll" &&
                       manifest.load_phase == anomaly::PluginLoadPhase::GameReady,
                   "Manifest version or entry fields were not materialized") && passed;
    passed = Check(manifest.api.major == 1 && manifest.api.minimum_minor == 0 &&
                       manifest.api.maximum_minor == 2,
                   "Manifest API range was not materialized") && passed;
    passed = Check(manifest.games == std::vector<std::string>{"nte"} &&
                       manifest.builds == std::vector<std::string>{"nte-win64-*"},
                   "Manifest game/build contract was not materialized") && passed;
    passed = Check(manifest.dependencies.size() == 1 &&
                       manifest.dependencies[0].id == "com.example.base" &&
                       !manifest.dependencies[0].optional &&
                       manifest.dependencies[0].version_range.Matches(
                           *anomaly::ParseSemanticVersion("1.5.0")),
                   "Manifest dependency was not materialized") && passed;
    passed = Check(manifest.services.size() == 2 &&
                       !manifest.services[0].optional && manifest.services[1].optional &&
                       manifest.capabilities.size() == 3,
                   "Manifest optional service or capability fields were not materialized") && passed;
    std::ifstream schema_file(ANOMALY_PLUGIN_MANIFEST_SCHEMA_FILE, std::ios::binary);
    const std::string source_schema{
        std::istreambuf_iterator<char>(schema_file), std::istreambuf_iterator<char>()};
    passed = Check(!source_schema.empty() &&
                       source_schema == anomaly::PluginManifestSchemaJson(),
                   "Embedded plugin manifest schema differs from its source artifact") && passed;
    return passed;
}

bool TestOptionalDefaults() {
    const auto result = anomaly::ParsePluginManifest(MinimalManifest());
    return Check(result.Ok(), "Minimal plugin manifest did not parse") &&
        Check(result.manifest && result.manifest->author.empty() &&
                  result.manifest->description.empty() &&
                  result.manifest->license.empty() &&
                  result.manifest->audience == anomaly::PluginAudience::User &&
                  result.manifest->dependencies.empty() &&
                  result.manifest->services.empty() &&
                  result.manifest->capabilities.empty(),
              "Optional manifest fields did not default to empty");
}

bool TestNteTeleportAudience() {
    std::ifstream manifest_file(ANOMALY_NTE_TELEPORT_MANIFEST_FILE, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(manifest_file), std::istreambuf_iterator<char>()};
    const auto result = anomaly::ParsePluginManifest(source);
    return Check(result.Ok() && result.manifest &&
            result.manifest->id == "anomaly.builtin.nte-teleport" &&
            result.manifest->name == "Teleport" &&
            !result.manifest->description.empty() &&
            result.manifest->audience == anomaly::PluginAudience::Developer,
        "Teleport metadata must remain complete and restricted to developer mode");
}

bool TestInputGates() {
    bool passed = true;

    const std::string oversized(anomaly::kMaximumPluginManifestBytes + 1, ' ');
    auto result = anomaly::ParsePluginManifest(oversized);
    passed = Check(!result.manifest &&
                       FindDiagnostic(result, anomaly::PluginManifestErrorCode::DocumentTooLarge),
                   "Oversized manifest was not rejected") && passed;

    std::string invalid_utf8 = "{\"name\":\"";
    invalid_utf8.push_back(static_cast<char>(0xc0));
    invalid_utf8.push_back(static_cast<char>(0xaf));
    invalid_utf8 += "\"}";
    result = anomaly::ParsePluginManifest(invalid_utf8);
    const auto* utf8 = FindDiagnostic(result, anomaly::PluginManifestErrorCode::InvalidUtf8);
    passed = Check(utf8 != nullptr && utf8->source_offset == 9,
                   "Invalid UTF-8 did not produce its byte offset") && passed;

    const std::string bom = "\xef\xbb\xbf{}";
    result = anomaly::ParsePluginManifest(bom);
    passed = Check(FindDiagnostic(result, anomaly::PluginManifestErrorCode::InvalidUtf8),
                   "UTF-8 BOM was accepted") && passed;

    constexpr char raw_null[] = "{\0}";
    result = anomaly::ParsePluginManifest(
        std::string_view(raw_null, sizeof(raw_null) - 1));
    const auto* null = FindDiagnostic(result, anomaly::PluginManifestErrorCode::EmbeddedNull);
    passed = Check(null != nullptr && null->source_offset == 1,
                   "Raw NUL did not produce its byte offset") && passed;

    const auto escaped_null = ReplaceOnce(
        ValidManifest(), "\"name\": \"My Plugin\"",
        "\"name\": \"My\\u0000Plugin\"");
    result = anomaly::ParsePluginManifest(escaped_null);
    passed = Check(FindDiagnostic(
                       result, anomaly::PluginManifestErrorCode::EmbeddedNull, "/name"),
                   "Escaped U+0000 was not rejected after JSON decoding") && passed;

    result = anomaly::ParsePluginManifest("{");
    const auto* syntax = FindDiagnostic(result, anomaly::PluginManifestErrorCode::JsonSyntax);
    passed = Check(syntax != nullptr && syntax->source_offset != anomaly::kUnknownManifestOffset,
                   "Malformed JSON did not produce a syntax offset") && passed;

    const auto duplicate_root = ReplaceOnce(
        ValidManifest(), "{", "{\n  \"\\u0073chemaVersion\": 1,");
    result = anomaly::ParsePluginManifest(duplicate_root);
    passed = Check(FindDiagnostic(
                       result, anomaly::PluginManifestErrorCode::DuplicateJsonKey,
                       "/schemaVersion"),
                   "Escaped-equivalent duplicate JSON key was not rejected") && passed;

    const auto duplicate_nested = ReplaceOnce(
        ValidManifest(), "\"major\": 1", "\"major\": 1, \"major\": 1");
    result = anomaly::ParsePluginManifest(duplicate_nested);
    passed = Check(FindDiagnostic(
                       result, anomaly::PluginManifestErrorCode::DuplicateJsonKey,
                       "/api/major"),
                   "Nested duplicate JSON key was not rejected") && passed;

    std::string deeply_nested(anomaly::kMaximumPluginManifestNesting + 1, '[');
    deeply_nested += '0';
    deeply_nested.append(anomaly::kMaximumPluginManifestNesting + 1, ']');
    result = anomaly::ParsePluginManifest(deeply_nested);
    passed = Check(FindDiagnostic(
                       result, anomaly::PluginManifestErrorCode::DocumentTooDeep),
                   "Deeply nested manifest exceeded the parser budget") && passed;

    std::string too_complex{"["};
    for (std::size_t index = 0; index <= anomaly::kMaximumPluginManifestNodes; ++index) {
        if (index != 0) too_complex.push_back(',');
        too_complex.push_back('0');
    }
    too_complex.push_back(']');
    result = anomaly::ParsePluginManifest(too_complex);
    passed = Check(FindDiagnostic(
                       result, anomaly::PluginManifestErrorCode::DocumentTooComplex),
                   "Structurally complex manifest exceeded the parser budget") && passed;

    result = anomaly::ParsePluginManifest("{\"schemaVersion\":2,\"x\":1e10000}");
    passed = Check(FindDiagnostic(result, anomaly::PluginManifestErrorCode::JsonSyntax),
                   "Out-of-range JSON number escaped the parse result boundary") && passed;

    result = anomaly::ParsePluginManifest("[]");
    passed = Check(FindDiagnostic(result, anomaly::PluginManifestErrorCode::RootNotObject),
                   "Non-object manifest root was not rejected") && passed;

    const auto unsupported_v1 = ReplaceOnce(
        ValidManifest(), "\"schemaVersion\": 2", "\"schemaVersion\": 1");
    result = anomaly::ParsePluginManifest(unsupported_v1);
    passed = Check(FindDiagnostic(
                       result, anomaly::PluginManifestErrorCode::UnsupportedSchemaVersion,
                       "/schemaVersion"),
                   "Retired schema version was not rejected") && passed;
    return passed;
}

bool TestSchemaValidation() {
    bool passed = true;

    const auto missing_entry = ReplaceOnce(
        ValidManifest(), "  \"entry\": \"plugin.dll\",\n", "");
    auto result = anomaly::ParsePluginManifest(missing_entry);
    passed = Check(FindDiagnostic(result, anomaly::PluginManifestErrorCode::SchemaViolation),
                   "Missing required manifest field passed schema validation") && passed;

    const auto unknown_field = ReplaceOnce(
        ValidManifest(), "{", "{\n  \"unknown\": true,");
    result = anomaly::ParsePluginManifest(unknown_field);
    passed = Check(FindDiagnostic(result, anomaly::PluginManifestErrorCode::SchemaViolation),
                   "Unknown manifest field passed schema validation") && passed;

    const auto uppercase_id = ReplaceOnce(
        ValidManifest(), "com.example.my-plugin", "Com.Example.Plugin");
    result = anomaly::ParsePluginManifest(uppercase_id);
    passed = Check(FindDiagnostic(result, anomaly::PluginManifestErrorCode::SchemaViolation),
                   "Non-normalized plugin ID passed schema validation") && passed;

    const auto unknown_phase = ReplaceOnce(
        ValidManifest(), "\"game-ready\"", "\"renderer-ready\"");
    result = anomaly::ParsePluginManifest(unknown_phase);
    passed = Check(FindDiagnostic(result, anomaly::PluginManifestErrorCode::SchemaViolation),
                   "Unknown load phase passed schema validation") && passed;

    const auto unknown_audience = ReplaceOnce(
        ValidManifest(), "\"developer\"", "\"internal\"");
    result = anomaly::ParsePluginManifest(unknown_audience);
    passed = Check(FindDiagnostic(result, anomaly::PluginManifestErrorCode::SchemaViolation),
                   "Unknown plugin audience passed schema validation") && passed;

    const auto invalid_license = ReplaceOnce(
        ValidManifest(), "AGPL-3.0-only", "AGPL-3.0-only OR MIT");
    result = anomaly::ParsePluginManifest(invalid_license);
    passed = Check(FindDiagnostic(
                       result, anomaly::PluginManifestErrorCode::SchemaViolation, "/license"),
                   "Non-identifier plugin license passed schema validation") && passed;

    const auto duplicate_game = ReplaceOnce(
        ValidManifest(), "\"games\": [\"nte\"]", "\"games\": [\"nte\", \"nte\"]");
    result = anomaly::ParsePluginManifest(duplicate_game);
    passed = Check(FindDiagnostic(result, anomaly::PluginManifestErrorCode::SchemaViolation),
                   "Duplicate game ID passed schema validation") && passed;

    const auto malformed_game = ReplaceOnce(
        ValidManifest(), "\"games\": [\"nte\"]", "\"games\": [\"nte-\"]");
    result = anomaly::ParsePluginManifest(malformed_game);
    passed = Check(FindDiagnostic(
                       result, anomaly::PluginManifestErrorCode::SchemaViolation,
                       "/games/0"),
                   "Malformed game ID passed schema validation") && passed;

    std::string games{"["};
    for (int index = 0; index < 17; ++index) {
        if (index != 0) games += ", ";
        games += "\"game-" + std::to_string(index) + "\"";
    }
    games.push_back(']');
    const auto too_many_games = ReplaceOnce(ValidManifest(), "[\"nte\"]", games);
    result = anomaly::ParsePluginManifest(too_many_games);
    passed = Check(FindDiagnostic(
                       result, anomaly::PluginManifestErrorCode::SchemaViolation, "/games"),
                   "Game array item limit was not enforced before schema validation") && passed;

    const auto empty_games = ReplaceOnce(
        ValidManifest(), "\"games\": [\"nte\"]", "\"games\": []");
    result = anomaly::ParsePluginManifest(empty_games);
    passed = Check(FindDiagnostic(result, anomaly::PluginManifestErrorCode::SchemaViolation),
                   "Empty game array passed schema validation") && passed;

    const auto long_name = ReplaceOnce(
        ValidManifest(), "My Plugin", std::string(129, 'a'));
    result = anomaly::ParsePluginManifest(long_name);
    passed = Check(FindDiagnostic(result, anomaly::PluginManifestErrorCode::SchemaViolation),
                   "Oversized display name passed schema validation") && passed;

    const auto invalid_api_type = ReplaceOnce(
        ValidManifest(), "\"major\": 1", "\"major\": \"1\"");
    result = anomaly::ParsePluginManifest(invalid_api_type);
    passed = Check(FindDiagnostic(result, anomaly::PluginManifestErrorCode::SchemaViolation),
                   "Invalid API field type passed schema validation") && passed;
    return passed;
}

bool TestSemanticValidation() {
    bool passed = true;

    const auto invalid_version = ReplaceOnce(
        ValidManifest(), "1.2.0-rc.1+build.7", "1.2.0-01");
    auto result = anomaly::ParsePluginManifest(invalid_version);
    const auto* version = FindDiagnostic(
        result, anomaly::PluginManifestErrorCode::InvalidSemanticVersion, "/version");
    passed = Check(version != nullptr && version->value_offset == 6,
                   "Invalid SemVer did not preserve its value offset") && passed;

    const auto prefixed_version = ReplaceOnce(
        ValidManifest(), "1.2.0-rc.1+build.7", "v1.2.3");
    result = anomaly::ParsePluginManifest(prefixed_version);
    passed = Check(FindDiagnostic(
                       result, anomaly::PluginManifestErrorCode::InvalidSemanticVersion,
                       "/version"),
                   "SemVer grammar error was intercepted as a schema error") && passed;
    const auto incomplete_version = ReplaceOnce(
        ValidManifest(), "1.2.0-rc.1+build.7", "1.2");
    result = anomaly::ParsePluginManifest(incomplete_version);
    passed = Check(FindDiagnostic(
                       result, anomaly::PluginManifestErrorCode::InvalidSemanticVersion,
                       "/version"),
                   "Short SemVer grammar error was intercepted as a schema error") && passed;

    const auto invalid_range = ReplaceOnce(
        ValidManifest(), ">=1.0.0 <2.0.0", ">=01.0.0");
    result = anomaly::ParsePluginManifest(invalid_range);
    const auto* range = FindDiagnostic(
        result, anomaly::PluginManifestErrorCode::InvalidVersionRange,
        "/dependencies/0/version");
    passed = Check(range != nullptr && range->value_offset == 2,
                   "Invalid dependency range did not preserve its value offset") && passed;

    const auto unsupported_range = ReplaceOnce(
        ValidManifest(), ">=1.0.0 <2.0.0", "^1.0.0");
    result = anomaly::ParsePluginManifest(unsupported_range);
    passed = Check(FindDiagnostic(
                       result, anomaly::PluginManifestErrorCode::InvalidVersionRange,
                       "/dependencies/0/version"),
                   "Range grammar error was intercepted as a schema error") && passed;
    const auto empty_range = ReplaceOnce(
        ValidManifest(), ">=1.0.0 <2.0.0", "");
    result = anomaly::ParsePluginManifest(empty_range);
    passed = Check(FindDiagnostic(
                       result, anomaly::PluginManifestErrorCode::InvalidVersionRange,
                       "/dependencies/0/version"),
                   "Empty range grammar error was intercepted as a schema error") && passed;

    const auto invalid_api = ReplaceOnce(
        ValidManifest(), "\"minMinor\": 0, \"maxMinor\": 2",
        "\"minMinor\": 3, \"maxMinor\": 2");
    result = anomaly::ParsePluginManifest(invalid_api);
    passed = Check(FindDiagnostic(
                       result, anomaly::PluginManifestErrorCode::InvalidApiRange,
                       "/api/maxMinor"),
                   "Inverted API minor range was accepted") && passed;

    const auto unowned_build = ReplaceOnce(
        ValidManifest(), "\"builds\": [\"nte-win64-*\"]",
        "\"builds\": [\"win64-*\"]");
    result = anomaly::ParsePluginManifest(unowned_build);
    passed = Check(FindDiagnostic(
                       result, anomaly::PluginManifestErrorCode::UnownedBuildPattern,
                       "/builds/0") &&
                       FindDiagnostic(
                           result,
                           anomaly::PluginManifestErrorCode::GameWithoutBuildPattern,
                           "/games/0"),
                   "Unowned build pattern did not invalidate both sides of the target") &&
        passed;

    const auto bare_game_build = ReplaceOnce(
        ValidManifest(), "\"builds\": [\"nte-win64-*\"]",
        "\"builds\": [\"nte\"]");
    result = anomaly::ParsePluginManifest(bare_game_build);
    passed = Check(FindDiagnostic(
                       result, anomaly::PluginManifestErrorCode::UnownedBuildPattern,
                       "/builds/0"),
                   "Build pattern without the game delimiter was accepted") && passed;

    const auto empty_build_suffix = ReplaceOnce(
        ValidManifest(), "\"builds\": [\"nte-win64-*\"]",
        "\"builds\": [\"nte-\"]");
    result = anomaly::ParsePluginManifest(empty_build_suffix);
    passed = Check(FindDiagnostic(
                       result, anomaly::PluginManifestErrorCode::UnownedBuildPattern,
                       "/builds/0"),
                   "Build pattern with an empty suffix was accepted") && passed;

    const auto game_without_build = ReplaceOnce(
        ValidManifest(), "\"games\": [\"nte\"]",
        "\"games\": [\"nte\", \"fixture\"]");
    result = anomaly::ParsePluginManifest(game_without_build);
    passed = Check(FindDiagnostic(
                       result,
                       anomaly::PluginManifestErrorCode::GameWithoutBuildPattern,
                       "/games/1"),
                   "Declared game without a build pattern was accepted") && passed;

    auto overlapping_games = ReplaceOnce(
        ValidManifest(), "\"games\": [\"nte\"]",
        "\"games\": [\"foo\", \"foo-bar\"]");
    overlapping_games = ReplaceOnce(
        std::move(overlapping_games), "\"builds\": [\"nte-win64-*\"]",
        "\"builds\": [\"foo-*\", \"foo-bar-*\"]");
    result = anomaly::ParsePluginManifest(overlapping_games);
    passed = Check(FindDiagnostic(
                       result, anomaly::PluginManifestErrorCode::OverlappingGameId,
                       "/games/1"),
                   "Overlapping game IDs made build ownership ambiguous") && passed;

    auto self_dependency = ReplaceOnce(
        ValidManifest(), "com.example.base", "com.example.my-plugin");
    self_dependency = ReplaceOnce(self_dependency, ">=1.0.0 <2.0.0", ">=01.0.0");
    result = anomaly::ParsePluginManifest(self_dependency);
    passed = Check(FindDiagnostic(
                       result, anomaly::PluginManifestErrorCode::SelfDependency,
                       "/dependencies/0/id") &&
                       FindDiagnostic(
                           result, anomaly::PluginManifestErrorCode::InvalidVersionRange,
                           "/dependencies/0/version") && !result.manifest,
                   "Self-dependency and range errors were not aggregated") && passed;

    const auto duplicate_dependency = ReplaceOnce(
        ValidManifest(),
        R"JSON(    { "id": "com.example.base", "version": ">=1.0.0 <2.0.0", "optional": false })JSON",
        R"JSON(    { "id": "com.example.base", "version": ">=1.0.0 <2.0.0", "optional": false },
    { "id": "com.example.base", "version": ">=1.0.0 <2.0.0", "optional": false })JSON");
    result = anomaly::ParsePluginManifest(duplicate_dependency);
    passed = Check(FindDiagnostic(
                       result, anomaly::PluginManifestErrorCode::DuplicateDependency,
                       "/dependencies/1/id"),
                   "Duplicate dependency ID was not rejected") && passed;

    const auto duplicate_service = ReplaceOnce(
        ValidManifest(),
        R"JSON(    { "id": "anomaly.ui", "minVersion": 1 },)JSON",
        R"JSON(    { "id": "anomaly.ui", "minVersion": 1 },
    { "id": "anomaly.ui", "minVersion": 1 },)JSON");
    result = anomaly::ParsePluginManifest(duplicate_service);
    passed = Check(FindDiagnostic(
                       result, anomaly::PluginManifestErrorCode::DuplicateService,
                       "/services/1/id"),
                   "Duplicate service requirement ID was not rejected") && passed;
    return passed;
}

}  // namespace

int main() {
    const bool result = TestValidManifest() && TestOptionalDefaults() &&
        TestNteTeleportAudience() && TestInputGates() && TestSchemaValidation() &&
        TestSemanticValidation();
    if (result) std::cout << "plugin manifest schema and parser contracts passed\n";
    return result ? 0 : 1;
}
