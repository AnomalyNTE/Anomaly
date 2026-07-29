#include "anomaly/platform_ui_model.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <map>
#include <sstream>
#include <utility>

namespace anomaly {
namespace {

constexpr std::array<std::string_view, 7> kRequiredNteCompatibilityFeatures{
    "ue5.framework", "ue5.world", "ue5.names", "ue5.objects",
    "nte.session", "nte.player", "nte.entities"};

bool HasCompleteNteCompatibilityFeatureSet(
    const std::vector<NteFeatureSnapshot>& features) noexcept {
    return std::ranges::all_of(kRequiredNteCompatibilityFeatures, [&](std::string_view id) {
        const auto found = std::ranges::find(features, id, &NteFeatureSnapshot::id);
        return found != features.end() && found->available && found->validated;
    });
}

std::string Lower(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        result.push_back(static_cast<char>(std::tolower(character)));
    }
    return result;
}

bool ContainsInsensitive(std::string_view value, std::string_view needle) {
    if (needle.empty()) return true;
    return Lower(value).find(Lower(needle)) != std::string::npos;
}

std::string Join(const std::vector<std::string>& values, std::string_view separator) {
    std::string result;
    for (const auto& value : values) {
        if (!result.empty()) result.append(separator);
        result += value;
    }
    return result;
}

std::string CatalogEntryId(const PluginCatalogEntry& entry) {
    if (!entry.Id().empty()) return std::string(entry.Id());
    return entry.package_root.filename().string();
}

bool IsBlockingDependency(PluginDependencyState state) noexcept {
    return state != PluginDependencyState::Ready;
}

bool IsRejectedCatalog(PluginCatalogStatus status) noexcept {
    return status == PluginCatalogStatus::InvalidManifest ||
        status == PluginCatalogStatus::InvalidPackage ||
        status == PluginCatalogStatus::DuplicateId;
}

void ComputeSummary(PlatformUiSnapshot& snapshot) {
    snapshot.runtime_summary = {};
    snapshot.runtime_summary.installed = snapshot.installed_plugins.size();
    for (const auto& plugin : snapshot.installed_plugins) {
        if (plugin.IsRunning()) ++snapshot.runtime_summary.running;
        if (plugin.IsDisabled()) ++snapshot.runtime_summary.disabled;
        if (plugin.HasIssue()) ++snapshot.runtime_summary.issues;
    }
}

void SetCatalogState(InstalledPluginView& view, const PluginCatalogEntry& entry) {
    view.has_catalog_entry = true;
    view.catalog_status = entry.status;
    view.catalog_issues = entry.issues;
    if (!entry.issues.empty()) {
        view.catalog_reason = entry.issues.front().message;
    }
    if (entry.manifest) {
        const auto& manifest = *entry.manifest;
        if (view.id.empty()) view.id = manifest.id;
        if (view.name.empty()) view.name = manifest.name;
        view.description = manifest.description;
        if (view.author.empty()) view.author = manifest.author;
        // Repository updates compare against the installed package manifest.
        // A loaded descriptor may still describe the previous generation while
        // the catalog worker is publishing a package replacement.
        view.version = manifest.version.ToString();
        view.audience = manifest.audience;
        if (view.source.empty()) view.source = entry.entry_file;
        if (view.package_directory.empty()) view.package_directory = entry.package_root;
        view.dependency_ids.clear();
        view.dependency_ids.reserve(manifest.dependencies.size());
        for (const auto& dependency : manifest.dependencies) {
            view.dependency_ids.push_back(dependency.id);
        }
    }
    if (entry.compatibility) {
        view.compatibility_result = entry.compatibility;
        view.compatible = entry.compatibility->Compatible();
        view.compatibility_issues = entry.compatibility->issues;
        if (!view.compatible && view.catalog_reason.empty() &&
            !view.compatibility_issues.empty()) {
            view.catalog_reason = view.compatibility_issues.front().path;
        }
    }
}

void SetDependencyState(InstalledPluginView& view, const PluginDependencyPlan& plan) {
    const auto* node = plan.Find(view.id);
    if (node == nullptr) return;
    view.dependency_state = node->state;
    if (view.catalog_reason.empty() && !node->diagnostics.empty()) {
        view.catalog_reason = node->diagnostics.front();
    }
}

void ApplyMetadataState(
    InstalledPluginView& view,
    const PluginCatalogEntry* entry,
    const PluginEnablementDecision* decision) {
    if (entry != nullptr && IsRejectedCatalog(entry->status)) {
        view.enabled = false;
        view.state = entry->status == PluginCatalogStatus::InvalidManifest ||
                entry->status == PluginCatalogStatus::InvalidPackage ||
                entry->status == PluginCatalogStatus::DuplicateId
            ? "rejected"
            : "incompatible";
        if (view.status_reason.empty()) view.status_reason = view.catalog_reason;
        return;
    }
    if (entry != nullptr && entry->status == PluginCatalogStatus::Incompatible) {
        view.enabled = false;
        view.state = "incompatible";
        if (view.status_reason.empty()) view.status_reason = view.catalog_reason;
        return;
    }
    if (IsBlockingDependency(view.dependency_state)) {
        view.enabled = false;
        view.state = "dependency-blocked";
        if (view.status_reason.empty()) view.status_reason = view.catalog_reason;
        return;
    }
    if (decision != nullptr) {
        view.enablement_configured = decision->configured;
        view.enablement_default_enabled = decision->enabled;
        view.enabled = decision->enabled;
        if (!decision->enabled) {
            view.state = "disabled";
            if (view.status_reason.empty()) view.status_reason = decision->reason;
            return;
        }
    }
    if (!view.has_runtime_view && view.state.empty()) {
        view.state = "not-loaded";
        if (view.status_reason.empty()) view.status_reason = "catalog entry is not active";
    }
}

bool IsStartLike(PlatformUiPluginMutation mutation) noexcept {
    return mutation == PlatformUiPluginMutation::Start ||
        mutation == PlatformUiPluginMutation::Enable;
}

bool IsStopLike(PlatformUiPluginMutation mutation) noexcept {
    return mutation == PlatformUiPluginMutation::Stop ||
        mutation == PlatformUiPluginMutation::Disable;
}

void AddAffected(
    std::map<std::string, AffectedPlugin, std::less<>>& affected,
    std::string id,
    bool directly_requested,
    std::string reason) {
    if (id.empty()) return;
    // Keep the map key intact while moving the display object; argument evaluation
    // order is unspecified and moving `id` inline can otherwise produce an empty key.
    const std::string key = id;
    auto [iterator, inserted] = affected.emplace(
        key, AffectedPlugin{std::move(id), directly_requested, std::move(reason)});
    if (!inserted && directly_requested) iterator->second.directly_requested = true;
}

bool HasDependency(const InstalledPluginView& plugin, std::string_view id) noexcept {
    return std::find(plugin.dependency_ids.begin(), plugin.dependency_ids.end(), id) !=
        plugin.dependency_ids.end();
}

std::string StateSortKey(const InstalledPluginView& plugin) {
    return Lower(plugin.state);
}

std::string SortKey(const InstalledPluginView& plugin, PlatformUiPluginSort sort) {
    switch (sort) {
    case PlatformUiPluginSort::State: return StateSortKey(plugin);
    case PlatformUiPluginSort::Author: return Lower(plugin.author);
    case PlatformUiPluginSort::Name: return Lower(plugin.name);
    }
    return {};
}

}  // namespace

PlatformUiPluginState InstalledPluginView::UiState() const noexcept {
    if (has_catalog_entry) {
        if (catalog_status == PluginCatalogStatus::Incompatible) {
            return PlatformUiPluginState::Incompatible;
        }
        if (IsRejectedCatalog(catalog_status)) return PlatformUiPluginState::Rejected;
    }
    if (dependency_state != PluginDependencyState::Ready) {
        return PlatformUiPluginState::DependencyBlocked;
    }
    const std::string normalized = Lower(this->state);
    if (normalized == "active" || normalized == "running") return PlatformUiPluginState::Active;
    if (normalized == "loaded") return PlatformUiPluginState::Loaded;
    if (normalized == "disabled" || normalized == "not-loaded") {
        return PlatformUiPluginState::Disabled;
    }
    if (normalized == "faulted" || normalized == "fault") return PlatformUiPluginState::Faulted;
    if (normalized == "quarantined" || normalized == "quarantine") {
        return PlatformUiPluginState::Quarantined;
    }
    if (normalized == "waiting-for-service" || normalized == "waiting") {
        return PlatformUiPluginState::WaitingForService;
    }
    if (normalized == "rejected") return PlatformUiPluginState::Rejected;
    if (normalized == "dependency-blocked" || normalized == "blocked") {
        return PlatformUiPluginState::DependencyBlocked;
    }
    if (normalized == "incompatible") return PlatformUiPluginState::Incompatible;
    if (normalized == "stopping") return PlatformUiPluginState::Stopping;
    if (!enabled && !has_runtime_view) return PlatformUiPluginState::Disabled;
    return PlatformUiPluginState::Unknown;
}

bool InstalledPluginView::IsRunning() const noexcept {
    return enabled && UiState() == PlatformUiPluginState::Active;
}

bool InstalledPluginView::IsDisabled() const noexcept {
    const auto typed_state = UiState();
    // Rejected, blocked, and quarantined entries are issues,
    // not ordinary user-disabled entries, even though they cannot run.
    return typed_state == PlatformUiPluginState::Disabled ||
        (typed_state == PlatformUiPluginState::Unknown && !enabled && status_reason.empty());
}

bool InstalledPluginView::HasIssue() const noexcept {
    switch (UiState()) {
    case PlatformUiPluginState::Faulted:
    case PlatformUiPluginState::Quarantined:
    case PlatformUiPluginState::WaitingForService:
    case PlatformUiPluginState::Rejected:
    case PlatformUiPluginState::DependencyBlocked:
    case PlatformUiPluginState::Incompatible:
    case PlatformUiPluginState::Stopping:
        return true;
    default:
        break;
    }
    return enabled && !status_reason.empty() && !IsRunning();
}

std::string_view InstalledPluginView::Reason() const noexcept {
    if (!status_reason.empty()) return status_reason;
    if (!catalog_reason.empty()) return catalog_reason;
    switch (UiState()) {
    case PlatformUiPluginState::Rejected: return "catalog entry was rejected";
    case PlatformUiPluginState::Incompatible: return "plugin is incompatible with this runtime";
    case PlatformUiPluginState::DependencyBlocked: return "a dependency is unavailable";
    case PlatformUiPluginState::WaitingForService: return "required service is not ready";
    default: return {};
    }
}

RepositoryPluginInstallState ResolveRepositoryPluginInstallState(
    const InstalledPluginView* installed,
    const RepositoryPluginView& available) {
    RepositoryPluginInstallState state;
    state.installed = installed != nullptr;
    if (installed != nullptr) {
        const auto current = ParseSemanticVersion(installed->version);
        const auto candidate = ParseSemanticVersion(available.entry.version);
        state.update_available = current && candidate &&
            CompareSemanticVersionPrecedence(*current, *candidate) < 0;
    }
    return state;
}

void NteCompatibilitySnapshot::NormalizeLevel() noexcept {
    bool any_available = false;
    bool all_validated = true;
    for (const auto& feature : features) {
        any_available = any_available || feature.available;
        all_validated = all_validated && feature.available && feature.validated;
    }
    const bool required_features_complete =
        HasCompleteNteCompatibilityFeatureSet(features);
    if (level == NteCompatibilityLevel::Supported &&
        (!all_validated || !required_features_complete)) {
        level = any_available ? NteCompatibilityLevel::Partial : NteCompatibilityLevel::CoreOnly;
    }
    if (level == NteCompatibilityLevel::Partial && !any_available) {
        level = NteCompatibilityLevel::CoreOnly;
    }
}

const NteFeatureSnapshot* NteCompatibilitySnapshot::FindFeature(std::string_view id) const noexcept {
    const auto found = std::find_if(features.begin(), features.end(), [&](const auto& feature) {
        return feature.id == id;
    });
    return found == features.end() ? nullptr : &*found;
}

const InstalledPluginView* PlatformUiSnapshot::FindPlugin(std::string_view id) const noexcept {
    const auto found = std::find_if(installed_plugins.begin(), installed_plugins.end(),
        [&](const auto& plugin) { return plugin.id == id; });
    return found == installed_plugins.end() ? nullptr : &*found;
}

std::string_view ToString(PlatformUiRoute route) noexcept {
    switch (route) {
    case PlatformUiRoute::Plugins: return "plugins";
    case PlatformUiRoute::NteCompatibility: return "nte-compatibility";
    case PlatformUiRoute::Diagnostics: return "diagnostics";
    case PlatformUiRoute::Settings: return "settings";
    }
    return "unknown";
}

std::string_view ToString(PlatformUiPluginState state) noexcept {
    switch (state) {
    case PlatformUiPluginState::Unknown: return "unknown";
    case PlatformUiPluginState::Active: return "active";
    case PlatformUiPluginState::Loaded: return "loaded";
    case PlatformUiPluginState::Disabled: return "disabled";
    case PlatformUiPluginState::Faulted: return "faulted";
    case PlatformUiPluginState::Quarantined: return "quarantined";
    case PlatformUiPluginState::WaitingForService: return "waiting-for-service";
    case PlatformUiPluginState::Rejected: return "rejected";
    case PlatformUiPluginState::DependencyBlocked: return "dependency-blocked";
    case PlatformUiPluginState::Incompatible: return "incompatible";
    case PlatformUiPluginState::Stopping: return "stopping";
    }
    return "unknown";
}

std::string_view ToString(PlatformUiResultCode code) noexcept {
    switch (code) {
    case PlatformUiResultCode::None: return "none";
    case PlatformUiResultCode::Accepted: return "accepted";
    case PlatformUiResultCode::InvalidIntent: return "invalid-intent";
    case PlatformUiResultCode::DuplicateIntent: return "duplicate-intent";
    case PlatformUiResultCode::RevisionConflict: return "revision-conflict";
    case PlatformUiResultCode::SubjectNotFound: return "subject-not-found";
    case PlatformUiResultCode::PreflightRequired: return "preflight-required";
    case PlatformUiResultCode::PreflightBlocked: return "preflight-blocked";
    case PlatformUiResultCode::Busy: return "busy";
    case PlatformUiResultCode::ProviderUnavailable: return "provider-unavailable";
    case PlatformUiResultCode::BackendFailure: return "backend-failure";
    }
    return "invalid";
}

std::string_view ToString(PluginOperationReason reason) noexcept {
    switch (reason) {
    case PluginOperationReason::None: return "none";
    case PluginOperationReason::Active: return "active";
    case PluginOperationReason::Disabled: return "disabled";
    case PluginOperationReason::NotFound: return "not-found";
    case PluginOperationReason::Rejected: return "rejected";
    case PluginOperationReason::Incompatible: return "incompatible";
    case PluginOperationReason::DependencyBlocked: return "dependency-blocked";
    case PluginOperationReason::RevisionConflict: return "revision-conflict";
    case PluginOperationReason::DuplicateIntent: return "duplicate-intent";
    case PluginOperationReason::PreflightRequired: return "preflight-required";
    case PluginOperationReason::ProviderUnavailable: return "provider-unavailable";
    case PluginOperationReason::BackendFailure: return "backend-failure";
    case PluginOperationReason::GenerationChanged: return "generation-changed";
    case PluginOperationReason::NotSupported: return "not-supported";
    case PluginOperationReason::AlreadyPending: return "already-pending";
    }
    return "unknown";
}

PlatformUiPluginState ClassifyPluginState(std::string_view state) noexcept {
    InstalledPluginView view;
    view.state = std::string(state);
    view.has_runtime_view = true;
    return view.UiState();
}

PlatformUiPluginMutation ResolvePlatformUiMutation(
    const PlatformUiIntent& intent) noexcept {
    if (intent.kind == PlatformUiIntentKind::SetPluginEnabled) {
        return intent.bool_value ? PlatformUiPluginMutation::Enable
                                 : PlatformUiPluginMutation::Disable;
    }
    if (intent.kind == PlatformUiIntentKind::ReloadPlugin) {
        return PlatformUiPluginMutation::Reload;
    }
    if (intent.kind == PlatformUiIntentKind::ReloadAllInstalled) {
        return PlatformUiPluginMutation::ReloadAll;
    }
    if (intent.kind == PlatformUiIntentKind::SetPluginVisible) {
        return PlatformUiPluginMutation::SetVisible;
    }
    return intent.mutation;
}

bool IsMutationIntent(PlatformUiIntentKind kind) noexcept {
    return kind == PlatformUiIntentKind::PlanPluginMutation ||
        kind == PlatformUiIntentKind::SetPluginEnabled ||
        kind == PlatformUiIntentKind::ReloadPlugin ||
        kind == PlatformUiIntentKind::ReloadAllInstalled ||
        kind == PlatformUiIntentKind::SetPluginVisible;
}

bool MatchesPluginSearch(const InstalledPluginView& plugin, std::string_view search) noexcept {
    if (search.empty()) return true;
    return ContainsInsensitive(plugin.id, search) || ContainsInsensitive(plugin.name, search) ||
        ContainsInsensitive(plugin.description, search) ||
        ContainsInsensitive(plugin.author, search) || ContainsInsensitive(plugin.version, search) ||
        ContainsInsensitive(plugin.state, search) || ContainsInsensitive(plugin.status_reason, search) ||
        ContainsInsensitive(plugin.catalog_reason, search);
}

bool MatchesPluginFilter(
    const InstalledPluginView& plugin, PlatformUiPluginFilter filter) noexcept {
    switch (filter) {
    case PlatformUiPluginFilter::All: return true;
    case PlatformUiPluginFilter::Running: return plugin.IsRunning();
    case PlatformUiPluginFilter::Disabled: return plugin.IsDisabled();
    case PlatformUiPluginFilter::Issues: return plugin.HasIssue();
    }
    return false;
}

std::vector<InstalledPluginView> FilterAndSortPlugins(
    const std::vector<InstalledPluginView>& plugins,
    std::string_view search,
    PlatformUiPluginFilter filter,
    PlatformUiPluginSort sort,
    bool include_developer) {
    std::vector<InstalledPluginView> result;
    result.reserve(plugins.size());
    for (const auto& plugin : plugins) {
        if (!include_developer && plugin.audience == PluginAudience::Developer) continue;
        if (MatchesPluginSearch(plugin, search) && MatchesPluginFilter(plugin, filter)) {
            result.push_back(plugin);
        }
    }
    std::stable_sort(result.begin(), result.end(), [&](const auto& left, const auto& right) {
        const std::string left_key = SortKey(left, sort);
        const std::string right_key = SortKey(right, sort);
        if (left_key != right_key) return left_key < right_key;
        return Lower(left.id) < Lower(right.id);
    });
    return result;
}

std::vector<std::string> FilteredPluginIds(
    const std::vector<InstalledPluginView>& plugins,
    std::string_view search,
    PlatformUiPluginFilter filter,
    PlatformUiPluginSort sort,
    bool include_developer) {
    const auto filtered = FilterAndSortPlugins(
        plugins, search, filter, sort, include_developer);
    std::vector<std::string> result;
    result.reserve(filtered.size());
    for (const auto& plugin : filtered) result.push_back(plugin.id);
    return result;
}

std::size_t PlatformUiPluginFilterCounts::For(PlatformUiPluginFilter filter) const noexcept {
    switch (filter) {
    case PlatformUiPluginFilter::All: return all;
    case PlatformUiPluginFilter::Running: return running;
    case PlatformUiPluginFilter::Disabled: return disabled;
    case PlatformUiPluginFilter::Issues: return issues;
    }
    return 0;
}

PlatformUiPluginFilterCounts CountFilteredPlugins(
    const std::vector<InstalledPluginView>& plugins, std::string_view search,
    bool include_developer) {
    PlatformUiPluginFilterCounts result;
    for (const auto& plugin : plugins) {
        if (!include_developer && plugin.audience == PluginAudience::Developer) continue;
        if (!MatchesPluginSearch(plugin, search)) continue;
        ++result.all;
        if (plugin.IsRunning()) ++result.running;
        if (plugin.IsDisabled()) ++result.disabled;
        if (plugin.HasIssue()) ++result.issues;
    }
    return result;
}

std::string ReconcilePluginSelection(
    const std::vector<InstalledPluginView>& sorted_plugins,
    std::string_view previous_selection) {
    if (!previous_selection.empty()) {
        const auto found = std::find_if(sorted_plugins.begin(), sorted_plugins.end(),
            [&](const auto& plugin) { return plugin.id == previous_selection; });
        if (found != sorted_plugins.end()) return std::string(previous_selection);
    }
    return sorted_plugins.empty() ? std::string{} : sorted_plugins.front().id;
}

void ReconcileUiStateSelection(
    const PlatformUiSnapshot& snapshot, PlatformUiState& state,
    bool include_developer) {
    const auto sorted = FilterAndSortPlugins(
        snapshot.installed_plugins, state.search, state.plugin_filter, state.plugin_sort,
        include_developer);
    state.selected_plugin_id = ReconcilePluginSelection(sorted, state.selected_plugin_id);
}

PlatformUiSnapshot BuildPlatformUiSnapshot(
    std::uint64_t revision, std::vector<ue5mem::PluginView> runtime_plugins) {
    PlatformUiSnapshot snapshot;
    snapshot.revision = revision;
    snapshot.installed_plugins.reserve(runtime_plugins.size());
    for (auto& runtime : runtime_plugins) {
        InstalledPluginView view(std::move(runtime));
        view.has_catalog_entry = false;
        view.catalog_status = PluginCatalogStatus::Valid;
        snapshot.installed_plugins.push_back(std::move(view));
    }
    std::sort(snapshot.installed_plugins.begin(), snapshot.installed_plugins.end(),
        [](const auto& left, const auto& right) { return Lower(left.id) < Lower(right.id); });
    ComputeSummary(snapshot);
    return snapshot;
}

PlatformUiSnapshot BuildPlatformUiSnapshot(
    std::uint64_t revision,
    std::vector<ue5mem::PluginView> runtime_plugins,
    const PluginCatalogSnapshot& catalog,
    const PluginDependencyPlan& dependencies,
    const std::map<std::string, PluginEnablementDecision, std::less<>>& enablement,
    const PluginDisplayNameMap& display_names,
    const PluginDescriptionMap& descriptions) {
    PlatformUiSnapshot snapshot;
    snapshot.revision = revision;

    std::map<std::string, InstalledPluginView, std::less<>> merged;
    for (auto& runtime : runtime_plugins) {
        if (runtime.id.empty()) continue;
        InstalledPluginView view(std::move(runtime));
        view.catalog_status = PluginCatalogStatus::Valid;
        merged[view.id] = std::move(view);
    }

    for (const auto& entry : catalog.Entries()) {
        const std::string id = CatalogEntryId(entry);
        if (id.empty()) continue;
        auto found = merged.find(id);
        if (found == merged.end()) {
            InstalledPluginView view;
            view.id = id;
            view.name = entry.manifest ? entry.manifest->name : id;
            view.author = entry.manifest ? entry.manifest->author : std::string{};
            view.version = entry.manifest ? entry.manifest->version.ToString() : std::string{};
            view.source = entry.entry_file;
            view.package_directory = entry.package_root;
            view.state.clear();
            view.enabled = false;
            view.visible = false;
            found = merged.emplace(id, std::move(view)).first;
        }
        auto& view = found->second;
        SetCatalogState(view, entry);
        SetDependencyState(view, dependencies);
        const auto decision = enablement.find(id);
        ApplyMetadataState(view, &entry, decision == enablement.end() ? nullptr : &decision->second);
    }

    for (auto& [id, view] : merged) {
        const auto display_name = display_names.find(id);
        if (display_name != display_names.end() && !display_name->second.empty()) {
            view.name = display_name->second;
        }
        const auto description = descriptions.find(id);
        if (description != descriptions.end() && !description->second.empty()) {
            view.description = description->second;
        }
        if (!view.has_catalog_entry) {
            if (view.state.empty()) view.state = view.enabled ? "active" : "disabled";
            continue;
        }
        if (view.state.empty()) view.state = view.has_runtime_view ? "loaded" : "not-loaded";
    }
    snapshot.installed_plugins.reserve(merged.size());
    for (auto& [id, view] : merged) snapshot.installed_plugins.push_back(std::move(view));
    std::sort(snapshot.installed_plugins.begin(), snapshot.installed_plugins.end(),
        [](const auto& left, const auto& right) { return Lower(left.id) < Lower(right.id); });
    ComputeSummary(snapshot);
    return snapshot;
}

NteCompatibilitySnapshot BuildNteCompatibilitySnapshot(
    std::optional<BuildFingerprint> fingerprint,
    std::optional<BuildProfile> profile,
    std::optional<ProfileResolutionSnapshot> resolution,
    std::vector<AvailableServiceVersion> services) {
    NteCompatibilitySnapshot result;
    result.services = std::move(services);
    if (fingerprint) {
        result.game_id = fingerprint->game;
        result.build_id = fingerprint->id;
    }
    if (profile) {
        result.profile_source = profile->source.empty()
            ? profile->source_channel
            : profile->source.string();
        result.profile_hash = profile->source_hash;
        if (result.game_id.empty()) result.game_id = profile->game;
    }
    if (!resolution) {
        result.level = profile ? NteCompatibilityLevel::CoreOnly : NteCompatibilityLevel::Unknown;
        result.reason = profile ? "profile has not been resolved" : "no build profile is available";
        return result;
    }

    result.features.reserve(resolution->features.size());
    bool any_available = false;
    bool all_available = true;
    for (const auto& [id, feature] : resolution->features) {
        if (profile && profile->optional_features.contains(id)) continue;
        NteFeatureSnapshot item;
        item.id = id;
        item.available = feature.available;
        item.validated = feature.available;
        item.reason = feature.available ? "resolved" :
            (feature.missing_symbols.empty()
                ? "required symbols are unavailable"
                : "missing symbols: " + Join(feature.missing_symbols, ", "));
        any_available = any_available || item.available;
        all_available = all_available && item.available;
        result.features.push_back(std::move(item));
    }
    switch (resolution->state) {
    case ProfileResolutionState::Ready: {
        const bool required_features_complete =
            HasCompleteNteCompatibilityFeatureSet(result.features);
        if (all_available && required_features_complete) {
            result.level = NteCompatibilityLevel::Supported;
            result.reason = "all published features resolved";
        } else if (any_available) {
            result.level = NteCompatibilityLevel::Partial;
            result.reason = !required_features_complete
                ? "the required Phase 10 feature set is incomplete"
                : "some published features are unavailable";
        } else {
            result.level = NteCompatibilityLevel::CoreOnly;
            result.reason = "profile resolved without an available feature";
        }
        break;
    }
    case ProfileResolutionState::ProfileLoaded:
    case ProfileResolutionState::Degraded:
        result.level = any_available ? NteCompatibilityLevel::Partial : NteCompatibilityLevel::CoreOnly;
        result.reason = resolution->state == ProfileResolutionState::Degraded
            ? "profile resolution is degraded"
            : "profile matched but full readiness is not established";
        break;
    case ProfileResolutionState::NoProfile:
        result.level = NteCompatibilityLevel::CoreOnly;
        result.reason = "the running build is not known";
        break;
    }
    result.NormalizeLevel();
    return result;
}

PlatformUiAffectedSetPreflight PreflightPluginMutation(
    const PlatformUiSnapshot& snapshot, const PlatformUiIntent& intent) {
    PlatformUiAffectedSetPreflight result;
    result.mutation = ResolvePlatformUiMutation(intent);
    result.subject_id = intent.subject_id;

    std::map<std::string, AffectedPlugin, std::less<>> affected;
    const auto add_plugin = [&](std::string_view id, bool direct, std::string reason) {
        AddAffected(affected, std::string(id), direct, std::move(reason));
    };

    if (result.mutation == PlatformUiPluginMutation::ReloadAll) {
        for (const auto& plugin : snapshot.installed_plugins) {
            add_plugin(plugin.id, false, "batch reload");
        }
    } else {
        const auto* target = snapshot.FindPlugin(intent.subject_id);
        if (target == nullptr) {
            result.decision = PlatformUiPreflightDecision::Blocked;
            result.reason = "plugin was not found in the Installed snapshot";
            return result;
        }
        add_plugin(target->id, true, "requested plugin");
        if (IsStartLike(result.mutation) || result.mutation == PlatformUiPluginMutation::Reload) {
            if (target->UiState() == PlatformUiPluginState::Rejected ||
                target->UiState() == PlatformUiPluginState::Incompatible) {
                result.decision = PlatformUiPreflightDecision::Blocked;
                result.reason = std::string(ToString(target->UiState()));
            } else if (target->dependency_state != PluginDependencyState::Ready) {
                result.decision = PlatformUiPreflightDecision::Blocked;
                result.reason = "dependency graph blocks this operation";
            } else if (result.mutation == PlatformUiPluginMutation::Reload &&
                       !target->has_runtime_view) {
                result.decision = PlatformUiPreflightDecision::Blocked;
                result.reason = "plugin has no active generation to reload";
            }
            if (result.mutation == PlatformUiPluginMutation::Reload) {
                // ReloadPackages replaces the selected generation and every
                // transitive dependent that holds imports from it.
                bool changed = true;
                while (changed) {
                    changed = false;
                    for (const auto& plugin : snapshot.installed_plugins) {
                        if (affected.contains(plugin.id)) continue;
                        for (const auto& item : affected) {
                            if (HasDependency(plugin, item.first)) {
                                add_plugin(
                                    plugin.id, false,
                                    "dependent generation must reload");
                                changed = true;
                                break;
                            }
                        }
                    }
                }
            } else {
                // Starting a plugin requires its complete dependency closure.
                std::unordered_set<std::string> visiting;
                std::function<void(const InstalledPluginView&)> add_dependencies =
                    [&](const InstalledPluginView& plugin) {
                        if (!visiting.insert(plugin.id).second) return;
                        for (const auto& dependency_id : plugin.dependency_ids) {
                            const auto* dependency = snapshot.FindPlugin(dependency_id);
                            if (dependency == nullptr) {
                                result.decision = PlatformUiPreflightDecision::Blocked;
                                result.reason = "required dependency is missing: " + dependency_id;
                                continue;
                            }
                            add_plugin(dependency->id, false, "required dependency");
                            if (dependency->dependency_state != PluginDependencyState::Ready ||
                                dependency->UiState() == PlatformUiPluginState::Rejected ||
                                dependency->UiState() == PlatformUiPluginState::Incompatible) {
                                result.decision = PlatformUiPreflightDecision::Blocked;
                                result.reason = "required dependency is blocked: " + dependency->id;
                            }
                            add_dependencies(*dependency);
                        }
                    };
                add_dependencies(*target);
            }
        } else if (IsStopLike(result.mutation)) {
            // Stopping a dependency also stops all installed dependents first.
            bool changed = true;
            while (changed) {
                changed = false;
                for (const auto& plugin : snapshot.installed_plugins) {
                    if (affected.find(plugin.id) != affected.end()) continue;
                    for (const auto& item : affected) {
                        if (HasDependency(plugin, item.first)) {
                            add_plugin(plugin.id, false, "dependent plugin must stop first");
                            changed = true;
                            break;
                        }
                    }
                }
            }
        } else if (result.mutation == PlatformUiPluginMutation::SetVisible) {
            if (!target->visibility_control || !target->IsRunning()) {
                result.decision = PlatformUiPreflightDecision::Blocked;
                result.reason = "plugin does not expose an active visibility control";
            }
        }
    }

    for (auto& [id, item] : affected) result.affected.push_back(std::move(item));
    if (result.decision == PlatformUiPreflightDecision::Blocked) return result;
    if (result.affected.empty()) {
        result.decision = PlatformUiPreflightDecision::Blocked;
        result.reason = "no Installed plugins are available";
        return result;
    }
    const bool confirmation = result.affected.size() > 1 ||
        result.mutation == PlatformUiPluginMutation::ReloadAll;
    result.decision = confirmation
        ? PlatformUiPreflightDecision::ConfirmationRequired
        : PlatformUiPreflightDecision::Direct;
    if (confirmation && result.reason.empty()) {
        result.reason = "operation affects multiple plugins";
    }
    return result;
}

PlatformUiIntentValidation ValidatePlatformUiIntent(
    const PlatformUiSnapshot& snapshot,
    const PlatformUiIntent& intent,
    const std::unordered_set<std::uint64_t>& submitted_intents) {
    PlatformUiIntentValidation result;
    if (intent.intent_id != 0 && submitted_intents.contains(intent.intent_id)) {
        result.duplicate_intent = true;
        result.code = PlatformUiResultCode::DuplicateIntent;
        result.reason = "intent id has already been submitted";
        return result;
    }
    if (intent.intent_id != 0) {
        const auto duplicate = std::find_if(snapshot.operation_results.begin(),
            snapshot.operation_results.end(), [&](const auto& operation) {
                return operation.intent_id == intent.intent_id;
            });
        if (duplicate != snapshot.operation_results.end()) {
            result.duplicate_intent = true;
            result.code = PlatformUiResultCode::DuplicateIntent;
            result.reason = "intent id already has an operation result";
            return result;
        }
    }
    if (intent.expected_revision != snapshot.revision) {
        result.revision_conflict = true;
        result.code = PlatformUiResultCode::RevisionConflict;
        result.reason = "the UI snapshot revision is stale";
        return result;
    }

    switch (intent.kind) {
    case PlatformUiIntentKind::OpenRoute:
        result.accepted = true;
        result.code = PlatformUiResultCode::Accepted;
        return result;
    case PlatformUiIntentKind::ViewPluginLogs:
        if (!intent.subject_id.empty() && snapshot.FindPlugin(intent.subject_id) == nullptr) {
            result.subject_missing = true;
            result.code = PlatformUiResultCode::SubjectNotFound;
            result.reason = "plugin was not found in the Installed snapshot";
            return result;
        }
        result.accepted = true;
        result.code = PlatformUiResultCode::Accepted;
        return result;
    case PlatformUiIntentKind::PlanPluginMutation:
        if (intent.mutation == PlatformUiPluginMutation::None) {
            result.code = PlatformUiResultCode::InvalidIntent;
            result.reason = "planning intent must specify a plugin mutation";
            return result;
        }
        break;
    default:
        break;
    }

    result.preflight = PreflightPluginMutation(snapshot, intent);
    if (!result.preflight->Allowed()) {
        result.code = PlatformUiResultCode::PreflightBlocked;
        result.reason = result.preflight->reason;
        return result;
    }
    if (result.preflight->RequiresConfirmation() && !intent.confirmed) {
        result.code = PlatformUiResultCode::PreflightRequired;
        result.reason = result.preflight->reason;
        return result;
    }
    result.accepted = true;
    result.code = PlatformUiResultCode::Accepted;
    return result;
}

PlatformUiOperationResult MakeSubmittedOperation(
    const PlatformUiIntent& intent,
    const PlatformUiAffectedSetPreflight* preflight,
    std::uint64_t operation_id,
    std::uint64_t observed_revision) {
    PlatformUiOperationResult result;
    result.operation_id = operation_id;
    result.intent_id = intent.intent_id;
    result.expected_revision = intent.expected_revision;
    result.observed_revision = observed_revision;
    result.state = PlatformUiOperationState::Submitted;
    result.code = PlatformUiResultCode::Accepted;
    result.mutation = ResolvePlatformUiMutation(intent);
    result.subject_id = intent.subject_id;
    if (preflight != nullptr) {
        result.plugins.reserve(preflight->affected.size());
        for (const auto& affected : preflight->affected) {
            result.plugins.push_back({affected.id, PluginOperationOutcome::Skipped,
                PluginOperationReason::AlreadyPending, affected.reason, 0, 0, false});
        }
    }
    return result;
}

void FinalizeOperation(PlatformUiOperationResult& operation) {
    if (operation.plugins.empty()) {
        operation.state = PlatformUiOperationState::Failed;
        operation.code = PlatformUiResultCode::BackendFailure;
        operation.message = "operation returned no per-plugin result";
        operation.retryable = true;
        return;
    }
    std::size_t succeeded{};
    std::size_t failed{};
    std::size_t skipped{};
    for (const auto& plugin : operation.plugins) {
        succeeded += plugin.outcome == PluginOperationOutcome::Succeeded;
        failed += plugin.outcome == PluginOperationOutcome::Failed;
        skipped += plugin.outcome == PluginOperationOutcome::Skipped;
        operation.retryable = operation.retryable || plugin.retryable;
    }
    if (failed == 0 && skipped == 0) {
        operation.state = PlatformUiOperationState::Succeeded;
        operation.code = PlatformUiResultCode::Accepted;
        operation.message = "all plugins succeeded";
    } else if (succeeded == 0 && failed == 0 && skipped != 0) {
        operation.state = PlatformUiOperationState::Cancelled;
        operation.code = PlatformUiResultCode::BackendFailure;
        operation.message = "all plugins were skipped";
    } else if (succeeded == 0) {
        operation.state = PlatformUiOperationState::Failed;
        operation.code = PlatformUiResultCode::BackendFailure;
        operation.message = "all plugins failed";
    } else {
        operation.state = PlatformUiOperationState::PartiallyFailed;
        operation.code = PlatformUiResultCode::BackendFailure;
        operation.message = "some plugins did not succeed";
    }
}

PlatformUiModel::PlatformUiModel() = default;

PlatformUiModel::PlatformUiModel(PlatformUiSnapshot snapshot)
    : snapshot_(std::move(snapshot)) {
    ReconcileUiStateSelection(snapshot_, state_);
}

void PlatformUiModel::Publish(PlatformUiSnapshot snapshot) {
    snapshot_ = std::move(snapshot);
    ReconcileUiStateSelection(snapshot_, state_);
    pending_operations_.erase(
        std::remove_if(pending_operations_.begin(), pending_operations_.end(), [&](const auto& pending) {
            return std::any_of(snapshot_.operation_results.begin(), snapshot_.operation_results.end(),
                [&](const auto& completed) { return completed.operation_id == pending.operation_id; });
        }), pending_operations_.end());
}

std::vector<InstalledPluginView> PlatformUiModel::VisiblePlugins(
    bool include_developer) const {
    return FilterAndSortPlugins(snapshot_.installed_plugins, state_.search,
        state_.plugin_filter, state_.plugin_sort, include_developer);
}

PlatformUiPluginFilterCounts PlatformUiModel::FilterCounts(
    bool include_developer) const {
    return CountFilteredPlugins(
        snapshot_.installed_plugins, state_.search, include_developer);
}

PlatformUiIntent PlatformUiModel::NewIntent(
    PlatformUiIntentKind kind, std::string subject_id, PlatformUiPluginMutation mutation) {
    PlatformUiIntent intent;
    intent.intent_id = next_intent_id_++;
    intent.expected_revision = snapshot_.revision;
    intent.kind = kind;
    intent.mutation = mutation;
    intent.subject_id = std::move(subject_id);
    return intent;
}

PlatformUiIntentSubmission PlatformUiModel::Submit(PlatformUiIntent intent) {
    if (intent.intent_id == 0) intent.intent_id = next_intent_id_++;
    else next_intent_id_ = std::max(next_intent_id_, intent.intent_id + 1);
    if (intent.expected_revision == 0 && snapshot_.revision == 0) {
        intent.expected_revision = snapshot_.revision;
    }
    const auto validation = ValidatePlatformUiIntent(snapshot_, intent, submitted_intents_);
    PlatformUiIntentSubmission result;
    result.code = validation.code;
    result.reason = validation.reason;
    result.preflight = validation.preflight;
    if (!validation.accepted) return result;
    result.accepted = true;
    result.operation_id = next_operation_id_++;
    pending_operations_.push_back(MakeSubmittedOperation(
        intent,
        validation.preflight ? &*validation.preflight : nullptr,
        result.operation_id,
        snapshot_.revision));
    submitted_intents_.insert(intent.intent_id);
    return result;
}

void PlatformUiModel::ApplyOperationResult(PlatformUiOperationResult result) {
    pending_operations_.erase(
        std::remove_if(pending_operations_.begin(), pending_operations_.end(), [&](const auto& pending) {
            const bool operation_match = result.operation_id != 0 &&
                pending.operation_id == result.operation_id;
            const bool intent_match = result.intent_id != 0 &&
                pending.intent_id == result.intent_id;
            return operation_match || intent_match;
        }), pending_operations_.end());
    snapshot_.operation_results.push_back(std::move(result));
}

}  // namespace anomaly
