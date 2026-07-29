#include "anomaly/plugin_compatibility.hpp"

#include <algorithm>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace anomaly {
namespace {

std::string Join(const std::vector<std::string>& values) {
    std::string result;
    for (const std::string& value : values) {
        if (!result.empty()) result.push_back(',');
        result += value;
    }
    return result;
}

bool MatchesBuildPattern(std::string_view pattern, std::string_view build_id) noexcept {
    const std::size_t wildcard = pattern.find('*');
    if (wildcard == std::string_view::npos) return pattern == build_id;
    return wildcard != 0 && wildcard == pattern.size() - 1 &&
        build_id.starts_with(pattern.substr(0, wildcard));
}

bool StartsWithGameId(std::string_view value, std::string_view game_id) noexcept {
    return !game_id.empty() && value.size() > game_id.size() + 1 &&
        value.starts_with(game_id) && value[game_id.size()] == '-';
}

std::string JoinBuildPatternsForGame(
    const PluginManifest& manifest, std::string_view game_id) {
    std::vector<std::string> patterns;
    for (const std::string& pattern : manifest.builds) {
        if (StartsWithGameId(pattern, game_id)) patterns.push_back(pattern);
    }
    return Join(patterns);
}

const AvailablePluginVersion* FindPlugin(
    const PluginCompatibilityContext& context, std::string_view id) noexcept {
    const auto found = std::find_if(
        context.plugins.begin(), context.plugins.end(),
        [&](const AvailablePluginVersion& plugin) { return plugin.id == id; });
    return found == context.plugins.end() ? nullptr : &*found;
}

const AvailableServiceVersion* FindService(
    const PluginCompatibilityContext& context, std::string_view id) noexcept {
    const auto found = std::find_if(
        context.services.begin(), context.services.end(),
        [&](const AvailableServiceVersion& service) { return service.id == id; });
    return found == context.services.end() ? nullptr : &*found;
}

void AddIssue(
    PluginCompatibilityResult& result, PluginCompatibilityIssueCode code,
    bool blocking, std::string path, std::string subject,
    std::string expected, std::optional<std::string> actual) {
    result.issues.push_back({
        code,
        blocking,
        std::move(path),
        std::move(subject),
        std::move(expected),
        std::move(actual),
    });
}

bool ValidateContext(
    const PluginCompatibilityContext& context, PluginCompatibilityResult& result) {
    std::unordered_set<std::string_view> plugin_ids;
    plugin_ids.reserve(context.plugins.size());
    for (std::size_t index = 0; index < context.plugins.size(); ++index) {
        const AvailablePluginVersion& plugin = context.plugins[index];
        if (!plugin_ids.insert(plugin.id).second) {
            AddIssue(
                result, PluginCompatibilityIssueCode::DuplicateContextPlugin, true,
                "/context/plugins/" + std::to_string(index) + "/id", plugin.id,
                "unique plugin id", plugin.id);
        }
    }

    std::unordered_set<std::string_view> service_ids;
    service_ids.reserve(context.services.size());
    for (std::size_t index = 0; index < context.services.size(); ++index) {
        const AvailableServiceVersion& service = context.services[index];
        if (!service_ids.insert(service.id).second) {
            AddIssue(
                result, PluginCompatibilityIssueCode::DuplicateContextService, true,
                "/context/services/" + std::to_string(index) + "/id", service.id,
                "unique service id", service.id);
        }
    }
    return result.issues.empty();
}

void EvaluatePluginDependencies(
    const PluginManifest& manifest, const PluginCompatibilityContext& context,
    bool optional, PluginCompatibilityResult& result) {
    for (std::size_t index = 0; index < manifest.dependencies.size(); ++index) {
        const PluginDependencyManifest& dependency = manifest.dependencies[index];
        if (dependency.optional != optional) continue;
        const AvailablePluginVersion* available = FindPlugin(context, dependency.id);
        const std::string base = "/dependencies/" + std::to_string(index);
        if (available == nullptr) {
            AddIssue(
                result, PluginCompatibilityIssueCode::PluginDependencyMissing,
                !optional, base + "/id", dependency.id,
                dependency.version_expression, std::nullopt);
        } else if (!dependency.version_range.Matches(available->version)) {
            AddIssue(
                result, PluginCompatibilityIssueCode::PluginDependencyVersionMismatch,
                !optional, base + "/version", dependency.id,
                dependency.version_expression, available->version.ToString());
        }
    }
}

void EvaluateServices(
    const PluginManifest& manifest, const PluginCompatibilityContext& context,
    bool optional, PluginCompatibilityResult& result) {
    for (std::size_t index = 0; index < manifest.services.size(); ++index) {
        const PluginServiceRequirement& requirement = manifest.services[index];
        if (requirement.optional != optional) continue;
        const AvailableServiceVersion* available = FindService(context, requirement.id);
        const std::string base = "/services/" + std::to_string(index);
        if (available == nullptr) {
            AddIssue(
                result, PluginCompatibilityIssueCode::ServiceMissing,
                !optional, base + "/id", requirement.id,
                ">=" + std::to_string(requirement.minimum_version), std::nullopt);
        } else if (available->version < requirement.minimum_version) {
            AddIssue(
                result, PluginCompatibilityIssueCode::ServiceVersionTooLow,
                !optional, base + "/minVersion", requirement.id,
                ">=" + std::to_string(requirement.minimum_version),
                std::to_string(available->version));
        }
    }
}

}  // namespace

bool PluginCompatibilityResult::Compatible() const noexcept {
    return std::none_of(
        issues.begin(), issues.end(),
        [](const PluginCompatibilityIssue& issue) { return issue.blocking; });
}

bool PluginCompatibilityResult::Degraded() const noexcept {
    return Compatible() && !issues.empty();
}

PluginCompatibilityResult EvaluatePluginCompatibility(
    const PluginManifest& manifest, const PluginCompatibilityContext& context) {
    PluginCompatibilityResult result;
    if (!ValidateContext(context, result)) return result;

    if (context.api_major != manifest.api.major) {
        AddIssue(
            result, PluginCompatibilityIssueCode::ApiMajorMismatch, true,
            "/api/major", "api",
            std::to_string(manifest.api.major), std::to_string(context.api_major));
    } else if (context.api_minor < manifest.api.minimum_minor) {
        AddIssue(
            result, PluginCompatibilityIssueCode::ApiMinorBelowMinimum, true,
            "/api/minMinor", "api",
            ">=" + std::to_string(manifest.api.minimum_minor),
            std::to_string(context.api_minor));
    } else if (context.api_minor > manifest.api.maximum_minor) {
        AddIssue(
            result, PluginCompatibilityIssueCode::ApiMinorAboveMaximum, true,
            "/api/maxMinor", "api",
            "<=" + std::to_string(manifest.api.maximum_minor),
            std::to_string(context.api_minor));
    }

    bool game_matches{};
    if (!context.game_id) {
        AddIssue(
            result, PluginCompatibilityIssueCode::CurrentGameUnknown, true,
            "/games", "game", Join(manifest.games), std::nullopt);
    } else {
        game_matches =
            std::find(manifest.games.begin(), manifest.games.end(), *context.game_id) !=
            manifest.games.end();
    }
    if (context.game_id && !game_matches) {
        AddIssue(
            result, PluginCompatibilityIssueCode::GameMismatch, true,
            "/games", "game",
            Join(manifest.games), *context.game_id);
    } else {
        if (game_matches && !context.build_id) {
            AddIssue(
                result, PluginCompatibilityIssueCode::CurrentBuildUnknown, true,
                "/builds", "build", Join(manifest.builds), std::nullopt);
        } else if (game_matches) {
            if (context.build_id->find('*') != std::string::npos ||
                !StartsWithGameId(*context.build_id, *context.game_id)) {
                AddIssue(
                    result,
                    PluginCompatibilityIssueCode::GameBuildIdentityMismatch, true,
                    "/context/buildId", "build identity",
                    *context.game_id + "-<build>", *context.build_id);
            } else {
                const bool build_matches = std::any_of(
                    manifest.builds.begin(), manifest.builds.end(),
                    [&](const std::string& pattern) {
                        return StartsWithGameId(pattern, *context.game_id) &&
                            MatchesBuildPattern(pattern, *context.build_id);
                    });
                if (!build_matches) {
                    AddIssue(
                        result, PluginCompatibilityIssueCode::BuildMismatch, true,
                        "/builds", "build",
                        JoinBuildPatternsForGame(manifest, *context.game_id),
                        *context.build_id);
                }
            }
        }
    }

    EvaluatePluginDependencies(manifest, context, false, result);
    EvaluateServices(manifest, context, false, result);
    EvaluatePluginDependencies(manifest, context, true, result);
    EvaluateServices(manifest, context, true, result);

    return result;
}

}  // namespace anomaly
