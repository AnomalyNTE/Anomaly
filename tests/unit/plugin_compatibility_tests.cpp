#include "anomaly/plugin_compatibility.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

anomaly::SemanticVersion Version(std::string_view text) {
    anomaly::SemVerParseError error;
    auto version = anomaly::ParseSemanticVersion(text, &error);
    if (!version) throw std::runtime_error("invalid test version: " + error.message);
    return std::move(*version);
}

anomaly::SemanticVersionRange Range(std::string_view text) {
    anomaly::SemVerParseError error;
    auto range = anomaly::ParseSemanticVersionRange(text, &error);
    if (!range) throw std::runtime_error("invalid test range: " + error.message);
    return std::move(*range);
}

anomaly::PluginManifest Manifest() {
    anomaly::PluginManifest manifest;
    manifest.id = "com.example.plugin";
    manifest.api = {1, 0, 0};
    manifest.games = {"nte", "fixture"};
    manifest.builds = {"nte-win64-*", "fixture-exact"};
    manifest.dependencies.push_back({
        "com.example.base",
        ">=1.0.0 <2.0.0",
        Range(">=1.0.0 <2.0.0"),
        false,
    });
    manifest.dependencies.push_back({
        "com.example.optional",
        ">=2.0.0 <3.0.0",
        Range(">=2.0.0 <3.0.0"),
        true,
    });
    manifest.services.push_back({"anomaly.log", 2, false});
    manifest.services.push_back({"anomaly.ui", 3, true});
    return manifest;
}

anomaly::PluginCompatibilityContext Context() {
    anomaly::PluginCompatibilityContext context;
    context.api_major = 1;
    context.api_minor = 0;
    context.game_id = "nte";
    context.build_id = "nte-win64-20260716";
    context.plugins.push_back({"com.example.base", Version("1.5.0+sha.7")});
    context.plugins.push_back({"com.example.optional", Version("2.1.0")});
    context.services.push_back({"anomaly.log", 2});
    context.services.push_back({"anomaly.ui", 4});
    return context;
}

const anomaly::PluginCompatibilityIssue* FindIssue(
    const anomaly::PluginCompatibilityResult& result,
    anomaly::PluginCompatibilityIssueCode code) {
    const auto found = std::find_if(
        result.issues.begin(), result.issues.end(),
        [&](const anomaly::PluginCompatibilityIssue& issue) { return issue.code == code; });
    return found == result.issues.end() ? nullptr : &*found;
}

bool TestCompatibleBaselineAndApiBounds() {
    const auto manifest = Manifest();
    auto context = Context();
    bool passed = true;

    auto result = anomaly::EvaluatePluginCompatibility(manifest, context);
    passed = Check(result.Compatible() && !result.Degraded() && result.issues.empty(),
                   "API V1.0 baseline was rejected") && passed;

    context.api_minor = 1;
    result = anomaly::EvaluatePluginCompatibility(manifest, context);
    const auto* issue = FindIssue(
        result, anomaly::PluginCompatibilityIssueCode::ApiMinorAboveMaximum);
    passed = Check(!result.Compatible() && issue != nullptr &&
                       issue->path == "/api/maxMinor" && issue->expected == "<=0",
                   "API minor above V1.0 was not diagnosed") && passed;

    context.api_major = 4;
    context.api_minor = 1;
    result = anomaly::EvaluatePluginCompatibility(manifest, context);
    passed = Check(result.issues.size() == 1 &&
                       result.issues[0].code ==
                           anomaly::PluginCompatibilityIssueCode::ApiMajorMismatch,
                   "API major mismatch also emitted a minor mismatch") && passed;
    return passed;
}

bool TestGameAndBuildMatching() {
    auto manifest = Manifest();
    auto context = Context();
    bool passed = true;

    context.game_id = "fixture";
    context.build_id = "fixture-exact";
    auto result = anomaly::EvaluatePluginCompatibility(manifest, context);
    passed = Check(result.Compatible(), "Exact game/build alternative was rejected") && passed;

    context.build_id = "nte-win64-20260716";
    result = anomaly::EvaluatePluginCompatibility(manifest, context);
    auto* issue = FindIssue(
        result, anomaly::PluginCompatibilityIssueCode::GameBuildIdentityMismatch);
    passed = Check(!result.Compatible() && !result.Degraded() && issue != nullptr &&
                       issue->blocking && issue->path == "/context/buildId",
                   "A build owned by another supported game was accepted") && passed;

    context.game_id = "nte";
    context.build_id = "fixture-exact";
    result = anomaly::EvaluatePluginCompatibility(manifest, context);
    issue = FindIssue(
        result, anomaly::PluginCompatibilityIssueCode::GameBuildIdentityMismatch);
    passed = Check(!result.Compatible() && issue != nullptr && issue->blocking,
                   "Reverse game/build cross-product combination was accepted") && passed;

    context.game_id = "NTE";
    context.build_id = "unrelated";
    result = anomaly::EvaluatePluginCompatibility(manifest, context);
    issue = FindIssue(result, anomaly::PluginCompatibilityIssueCode::GameMismatch);
    passed = Check(!result.Compatible() && !result.Degraded() && issue != nullptr &&
                       issue->blocking &&
                       !FindIssue(result, anomaly::PluginCompatibilityIssueCode::BuildMismatch) &&
                       !FindIssue(
                           result,
                           anomaly::PluginCompatibilityIssueCode::GameBuildIdentityMismatch),
                   "Game mismatch did not suppress the unrelated build mismatch") && passed;

    context.game_id.reset();
    result = anomaly::EvaluatePluginCompatibility(manifest, context);
    issue = FindIssue(result, anomaly::PluginCompatibilityIssueCode::CurrentGameUnknown);
    passed = Check(!result.Compatible() && !result.Degraded() && issue != nullptr &&
                       issue->blocking &&
                       !FindIssue(result, anomaly::PluginCompatibilityIssueCode::BuildMismatch) &&
                       !FindIssue(
                           result,
                           anomaly::PluginCompatibilityIssueCode::GameBuildIdentityMismatch),
                   "Unknown game did not suppress build evaluation") && passed;

    context.game_id = "nte";
    context.build_id.reset();
    result = anomaly::EvaluatePluginCompatibility(manifest, context);
    issue = FindIssue(result, anomaly::PluginCompatibilityIssueCode::CurrentBuildUnknown);
    passed = Check(!result.Compatible() && !result.Degraded() && issue != nullptr &&
                       issue->blocking,
                   "Unknown build was treated as a normal mismatch") && passed;

    constexpr std::array compatible_builds{
        "nte-win64-",
        "nte-win64-20260716",
    };
    for (const std::string_view build : compatible_builds) {
        context.build_id = std::string(build);
        result = anomaly::EvaluatePluginCompatibility(manifest, context);
        passed = Check(result.Compatible(), "Anchored build prefix rejected a valid build") &&
            passed;
    }

    constexpr std::array invalid_identity_builds{
        "xnte-win64-20260716",
        "NTE-win64-20260716",
        "nte-win64-*",
    };
    for (const std::string_view build : invalid_identity_builds) {
        context.build_id = std::string(build);
        result = anomaly::EvaluatePluginCompatibility(manifest, context);
        issue = FindIssue(
            result, anomaly::PluginCompatibilityIssueCode::GameBuildIdentityMismatch);
        passed = Check(!result.Compatible() && issue != nullptr && issue->blocking,
                       "Game/build identity matching was not anchored and case-sensitive") &&
            passed;
    }

    constexpr std::array incompatible_builds{
        "nte-other-build",
        "nte-win64",
    };
    for (const std::string_view build : incompatible_builds) {
        context.build_id = std::string(build);
        result = anomaly::EvaluatePluginCompatibility(manifest, context);
        passed = Check(FindIssue(result, anomaly::PluginCompatibilityIssueCode::BuildMismatch),
                       "Build matching was not anchored and case-sensitive") && passed;
    }

    manifest.games = {"nte"};
    manifest.builds = {"nte-win64-20260716"};
    context.build_id = "nte-win64-20260716-hotfix";
    result = anomaly::EvaluatePluginCompatibility(manifest, context);
    passed = Check(FindIssue(result, anomaly::PluginCompatibilityIssueCode::BuildMismatch),
                   "Exact build pattern matched a longer build ID") && passed;
    return passed;
}

bool TestInvalidContextIsRejectedBeforeEvaluation() {
    const auto manifest = Manifest();
    auto context = Context();
    context.plugins.push_back({"com.example.base", Version("2.0.0")});
    context.services.push_back({"anomaly.log", 1});

    auto result = anomaly::EvaluatePluginCompatibility(manifest, context);
    bool passed = Check(!result.Compatible() && !result.Degraded() &&
                            result.issues.size() == 2,
                        "Duplicate context entries were not rejected as one invalid snapshot");
    if (result.issues.size() != 2) return false;

    const auto& plugin_issue = result.issues[0];
    passed = Check(
                 plugin_issue.code ==
                         anomaly::PluginCompatibilityIssueCode::DuplicateContextPlugin &&
                     plugin_issue.blocking &&
                     plugin_issue.path == "/context/plugins/2/id" &&
                     plugin_issue.subject == "com.example.base" &&
                     plugin_issue.actual == "com.example.base",
                 "Duplicate plugin context diagnostic is unstable") && passed;

    const auto& service_issue = result.issues[1];
    passed = Check(
                 service_issue.code ==
                         anomaly::PluginCompatibilityIssueCode::DuplicateContextService &&
                     service_issue.blocking &&
                     service_issue.path == "/context/services/2/id" &&
                     service_issue.subject == "anomaly.log" &&
                     service_issue.actual == "anomaly.log",
                 "Duplicate service context diagnostic is unstable") && passed;

    std::swap(context.plugins[0], context.plugins[2]);
    std::swap(context.services[0], context.services[2]);
    result = anomaly::EvaluatePluginCompatibility(manifest, context);
    passed = Check(
                 !result.Compatible() && !result.Degraded() &&
                     result.issues.size() == 2 && result.issues[0].blocking &&
                     result.issues[1].blocking &&
                     result.issues[0].code ==
                         anomaly::PluginCompatibilityIssueCode::DuplicateContextPlugin &&
                     result.issues[1].code ==
                         anomaly::PluginCompatibilityIssueCode::DuplicateContextService,
                 "Duplicate context diagnostics depended on conflicting entry order") &&
        passed;
    return passed;
}

bool TestPluginDependencies() {
    auto manifest = Manifest();
    auto context = Context();
    bool passed = true;

    auto result = anomaly::EvaluatePluginCompatibility(manifest, context);
    passed = Check(result.Compatible() && result.issues.empty(),
                   "Compatible plugin dependencies were rejected") && passed;

    context.plugins.erase(context.plugins.begin());
    result = anomaly::EvaluatePluginCompatibility(manifest, context);
    const auto* issue = FindIssue(
        result, anomaly::PluginCompatibilityIssueCode::PluginDependencyMissing);
    passed = Check(!result.Compatible() && issue != nullptr && issue->blocking &&
                       issue->path == "/dependencies/0/id" && !issue->actual,
                   "Missing required plugin dependency was not blocking") && passed;

    context = Context();
    context.plugins[0].version = Version("2.0.0");
    result = anomaly::EvaluatePluginCompatibility(manifest, context);
    issue = FindIssue(
        result, anomaly::PluginCompatibilityIssueCode::PluginDependencyVersionMismatch);
    passed = Check(!result.Compatible() && issue != nullptr && issue->blocking &&
                       issue->path == "/dependencies/0/version" &&
                       issue->actual == "2.0.0",
                   "Required plugin version mismatch was not blocking") && passed;

    context.plugins[0].version = Version("1.5.0-beta.1");
    result = anomaly::EvaluatePluginCompatibility(manifest, context);
    passed = Check(FindIssue(
                       result,
                       anomaly::PluginCompatibilityIssueCode::PluginDependencyVersionMismatch),
                   "Stable plugin range implicitly accepted a prerelease") && passed;

    context = Context();
    context.plugins.erase(context.plugins.begin() + 1);
    result = anomaly::EvaluatePluginCompatibility(manifest, context);
    issue = FindIssue(result, anomaly::PluginCompatibilityIssueCode::PluginDependencyMissing);
    passed = Check(result.Compatible() && result.Degraded() && issue != nullptr &&
                       !issue->blocking && issue->path == "/dependencies/1/id",
                   "Missing optional plugin dependency blocked compatibility") && passed;

    context = Context();
    context.plugins[1].version = Version("3.0.0");
    result = anomaly::EvaluatePluginCompatibility(manifest, context);
    issue = FindIssue(
        result, anomaly::PluginCompatibilityIssueCode::PluginDependencyVersionMismatch);
    passed = Check(result.Compatible() && result.Degraded() && issue != nullptr &&
                       !issue->blocking && issue->path == "/dependencies/1/version",
                   "Optional plugin version mismatch blocked compatibility") && passed;

    manifest.dependencies.resize(1);
    manifest.dependencies[0].version_expression = ">=1.5.0-beta.1 <1.5.0";
    manifest.dependencies[0].version_range = Range(">=1.5.0-beta.1 <1.5.0");
    context = Context();
    context.plugins.resize(1);
    context.plugins[0].version = Version("1.5.0-beta.2");
    result = anomaly::EvaluatePluginCompatibility(manifest, context);
    passed = Check(result.Compatible(), "Explicit prerelease range rejected its core version") &&
        passed;
    context.plugins[0].version = Version("1.5.1-alpha");
    result = anomaly::EvaluatePluginCompatibility(manifest, context);
    passed = Check(FindIssue(
                       result,
                       anomaly::PluginCompatibilityIssueCode::PluginDependencyVersionMismatch),
                   "Prerelease opt-in escaped its core version") && passed;

    manifest.dependencies[0].version_expression = "=1.5.0+expected";
    manifest.dependencies[0].version_range = Range("=1.5.0+expected");
    context.plugins[0].version = Version("1.5.0+different");
    result = anomaly::EvaluatePluginCompatibility(manifest, context);
    passed = Check(result.Compatible(), "Build metadata changed dependency precedence") && passed;
    return passed;
}

bool TestServiceRequirements() {
    const auto manifest = Manifest();
    auto context = Context();
    bool passed = true;

    context.services.erase(context.services.begin());
    auto result = anomaly::EvaluatePluginCompatibility(manifest, context);
    const auto* issue = FindIssue(result, anomaly::PluginCompatibilityIssueCode::ServiceMissing);
    passed = Check(!result.Compatible() && issue != nullptr && issue->blocking &&
                       issue->path == "/services/0/id" && !issue->actual,
                   "Missing required service was not blocking") && passed;

    context = Context();
    context.services[0].version = 1;
    result = anomaly::EvaluatePluginCompatibility(manifest, context);
    issue = FindIssue(result, anomaly::PluginCompatibilityIssueCode::ServiceVersionTooLow);
    passed = Check(!result.Compatible() && issue != nullptr && issue->blocking &&
                       issue->path == "/services/0/minVersion",
                   "Required service version mismatch was not blocking") && passed;

    for (const std::uint32_t version : {2U, 5U}) {
        context = Context();
        context.services[0].version = version;
        result = anomaly::EvaluatePluginCompatibility(manifest, context);
        passed = Check(result.Compatible() && result.issues.empty(),
                       "Compatible service minimum version was rejected") && passed;
    }

    context = Context();
    context.services.erase(context.services.begin() + 1);
    result = anomaly::EvaluatePluginCompatibility(manifest, context);
    issue = FindIssue(result, anomaly::PluginCompatibilityIssueCode::ServiceMissing);
    passed = Check(result.Compatible() && result.Degraded() && issue != nullptr &&
                       !issue->blocking && issue->path == "/services/1/id",
                   "Missing optional service blocked compatibility") && passed;

    context = Context();
    context.services[1].version = 2;
    result = anomaly::EvaluatePluginCompatibility(manifest, context);
    issue = FindIssue(result, anomaly::PluginCompatibilityIssueCode::ServiceVersionTooLow);
    passed = Check(result.Compatible() && result.Degraded() && issue != nullptr &&
                       !issue->blocking && issue->path == "/services/1/minVersion",
                   "Optional service version mismatch blocked compatibility") && passed;
    return passed;
}

bool TestAggregateOrdering() {
    const auto manifest = Manifest();
    auto context = Context();
    context.api_major = 4;
    context.build_id = "nte-unknown-build";
    context.plugins.clear();
    context.services.clear();

    const auto result = anomaly::EvaluatePluginCompatibility(manifest, context);
    constexpr std::array expected{
        anomaly::PluginCompatibilityIssueCode::ApiMajorMismatch,
        anomaly::PluginCompatibilityIssueCode::BuildMismatch,
        anomaly::PluginCompatibilityIssueCode::PluginDependencyMissing,
        anomaly::PluginCompatibilityIssueCode::ServiceMissing,
        anomaly::PluginCompatibilityIssueCode::PluginDependencyMissing,
        anomaly::PluginCompatibilityIssueCode::ServiceMissing,
    };
    bool passed = Check(result.issues.size() == expected.size(),
                        "Compatibility aggregation lost or duplicated issues");
    if (result.issues.size() != expected.size()) return false;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        passed = Check(result.issues[index].code == expected[index],
                       "Compatibility issue ordering is unstable") && passed;
    }
    const std::size_t blocking = static_cast<std::size_t>(std::count_if(
        result.issues.begin(), result.issues.end(),
        [](const anomaly::PluginCompatibilityIssue& issue) { return issue.blocking; }));
    return Check(!result.Compatible() && !result.Degraded() && blocking == 4,
                 "Aggregate blocking/advisory disposition is incorrect") && passed;
}

}  // namespace

int main() {
    const bool result = TestCompatibleBaselineAndApiBounds() &&
        TestGameAndBuildMatching() && TestInvalidContextIsRejectedBeforeEvaluation() &&
        TestPluginDependencies() &&
        TestServiceRequirements() && TestAggregateOrdering();
    if (result) std::cout << "plugin compatibility contracts passed\n";
    return result ? 0 : 1;
}
