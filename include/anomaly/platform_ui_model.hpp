#pragma once

#include "anomaly/plugin_catalog.hpp"
#include "anomaly/plugin_dependency_resolver.hpp"
#include "anomaly/plugin_enablement.hpp"
#include "anomaly/repository_coordinator.hpp"
#include "anomaly/symbol_resolver.hpp"
#include "plugin_manager.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace anomaly {

// These values are stable route keys. Display labels belong to the renderer.
enum class PlatformUiRoute : std::uint8_t {
    Plugins,
    NteCompatibility,
    Diagnostics,
    Settings,
};

enum class PlatformUiPluginTab : std::uint8_t {
    Installed,
    Available,
    Updates,
    ThirdParty,
};

enum class PlatformUiPluginFilter : std::uint8_t {
    All,
    Running,
    Disabled,
    Issues,
};

enum class PlatformUiPluginSort : std::uint8_t {
    Name,
    State,
    Author,
};

enum class PlatformUiDiagnosticsTab : std::uint8_t {
    Overview,
    PluginPerformance,
    Logs,
    Developer,
};

enum class PlatformUiPluginState : std::uint8_t {
    Unknown,
    Active,
    Loaded,
    Disabled,
    Faulted,
    Quarantined,
    WaitingForService,
    Rejected,
    DependencyBlocked,
    Incompatible,
    Stopping,
};

// Short aliases keep call sites readable while the prefixed names remain ABI-safe.
using PluginFilter = PlatformUiPluginFilter;
using PluginSort = PlatformUiPluginSort;
using PluginTab = PlatformUiPluginTab;
using PluginUiState = PlatformUiPluginState;

struct InstalledPluginView final : ue5mem::PluginView {
    InstalledPluginView() = default;
    explicit InstalledPluginView(ue5mem::PluginView runtime)
        : ue5mem::PluginView(std::move(runtime)), has_runtime_view(true) {}

    // Catalog and dependency information remains available even when no DLL loaded.
    bool has_catalog_entry{};
    bool has_runtime_view{};
    bool compatible{true};
    bool enablement_configured{};
    bool enablement_default_enabled{};
    PluginAudience audience{PluginAudience::User};
    std::string description;
    PluginCatalogStatus catalog_status{PluginCatalogStatus::Valid};
    PluginDependencyState dependency_state{PluginDependencyState::Ready};
    std::string catalog_reason;
    std::vector<PluginCatalogIssue> catalog_issues;
    std::optional<PluginCompatibilityResult> compatibility_result;
    std::vector<PluginCompatibilityIssue> compatibility_issues;
    std::vector<std::string> dependency_ids;

    [[nodiscard]] PlatformUiPluginState UiState() const noexcept;
    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] bool IsDisabled() const noexcept;
    [[nodiscard]] bool HasIssue() const noexcept;
    [[nodiscard]] std::string_view Reason() const noexcept;
};

struct RepositoryPluginInstallState final {
    bool installed{};
    bool update_available{};
};

[[nodiscard]] RepositoryPluginInstallState ResolveRepositoryPluginInstallState(
    const InstalledPluginView* installed,
    const RepositoryPluginView& available);

struct PlatformRuntimeSummary {
    std::size_t installed{};
    std::size_t running{};
    std::size_t issues{};
    std::size_t disabled{};
};

struct PlatformDiagnosticsSummary {
    std::string runtime_version;
    std::uint32_t process_id{};
    bool healthy{true};
    std::vector<std::string> recent_faults;
};

enum class NteCompatibilityLevel : std::uint8_t {
    Unknown,
    CoreOnly,
    Partial,
    Supported,
};

struct NteFeatureSnapshot {
    std::string id;
    bool available{};
    bool validated{};
    std::string reason;
};

using PluginDisplayNameMap = std::map<std::string, std::string, std::less<>>;
using PluginDescriptionMap = std::map<std::string, std::string, std::less<>>;

struct NteCompatibilitySnapshot {
    std::string game_id;
    std::string build_id;
    std::string profile_source;
    std::string profile_hash;
    NteCompatibilityLevel level{NteCompatibilityLevel::Unknown};
    std::string reason;
    std::vector<NteFeatureSnapshot> features;
    std::vector<AvailableServiceVersion> services;

    // Keeps Supported tied to the resolved, required feature set.
    void NormalizeLevel() noexcept;
    [[nodiscard]] const NteFeatureSnapshot* FindFeature(std::string_view id) const noexcept;
};

enum class PlatformUiIntentKind : std::uint8_t {
    PlanPluginMutation,
    SetPluginEnabled,
    ReloadPlugin,
    ReloadAllInstalled,
    SetPluginVisible,
    OpenRoute,
    ViewPluginLogs,
};

enum class PlatformUiPluginMutation : std::uint8_t {
    None,
    Start,
    Stop,
    Reload,
    Enable,
    Disable,
    SetVisible,
    ReloadAll,
};

struct PlatformUiState final {
    PlatformUiRoute route{PlatformUiRoute::Plugins};
    PlatformUiPluginTab plugin_tab{PlatformUiPluginTab::Installed};
    PlatformUiPluginFilter plugin_filter{PlatformUiPluginFilter::All};
    PlatformUiPluginSort plugin_sort{PlatformUiPluginSort::Name};
    PlatformUiDiagnosticsTab diagnostics_tab{PlatformUiDiagnosticsTab::Overview};
    std::string search;
    std::string selected_plugin_id;
    std::string diagnostics_plugin_id;
    std::string diagnostics_log_filter;
};

struct PlatformUiIntent final {
    std::uint64_t intent_id{};
    std::uint64_t expected_revision{};
    PlatformUiIntentKind kind{PlatformUiIntentKind::OpenRoute};
    PlatformUiPluginMutation mutation{PlatformUiPluginMutation::None};
    PlatformUiRoute route{PlatformUiRoute::Plugins};
    PlatformUiDiagnosticsTab diagnostics_tab{PlatformUiDiagnosticsTab::Overview};
    std::string subject_id;
    std::string value;
    bool bool_value{};
    bool confirmed{};
};

enum class PlatformUiPreflightDecision : std::uint8_t {
    Direct,
    ConfirmationRequired,
    Blocked,
    RevisionConflict,
};

struct AffectedPlugin final {
    std::string id;
    bool directly_requested{};
    std::string reason;
};

struct PlatformUiAffectedSetPreflight final {
    PlatformUiPreflightDecision decision{PlatformUiPreflightDecision::Direct};
    PlatformUiPluginMutation mutation{PlatformUiPluginMutation::None};
    std::string subject_id;
    std::string reason;
    std::vector<AffectedPlugin> affected;

    [[nodiscard]] bool Allowed() const noexcept {
        return decision != PlatformUiPreflightDecision::Blocked &&
            decision != PlatformUiPreflightDecision::RevisionConflict;
    }
    [[nodiscard]] bool RequiresConfirmation() const noexcept {
        return decision == PlatformUiPreflightDecision::ConfirmationRequired;
    }
};

using AffectedSetPreflight = PlatformUiAffectedSetPreflight;

enum class PluginOperationOutcome : std::uint8_t {
    Succeeded,
    Failed,
    Skipped,
};

enum class PluginOperationReason : std::uint16_t {
    None,
    Active,
    Disabled,
    NotFound,
    Rejected,
    Incompatible,
    DependencyBlocked,
    RevisionConflict,
    DuplicateIntent,
    PreflightRequired,
    ProviderUnavailable,
    BackendFailure,
    GenerationChanged,
    NotSupported,
    AlreadyPending,
};

struct PluginOperationResult final {
    std::string plugin_id;
    PluginOperationOutcome outcome{PluginOperationOutcome::Skipped};
    PluginOperationReason reason{PluginOperationReason::None};
    std::string message;
    std::uint64_t previous_generation{};
    std::uint64_t generation{};
    bool retryable{};
};

enum class PlatformUiOperationState : std::uint8_t {
    Idle,
    Submitted,
    Running,
    Succeeded,
    PartiallyFailed,
    Failed,
    Cancelled,
};

enum class PlatformUiResultCode : std::uint16_t {
    None,
    Accepted,
    InvalidIntent,
    DuplicateIntent,
    RevisionConflict,
    SubjectNotFound,
    PreflightRequired,
    PreflightBlocked,
    Busy,
    ProviderUnavailable,
    BackendFailure,
};

using PlatformUiOperationResultCode = PlatformUiResultCode;

struct PlatformUiOperationResult final {
    std::uint64_t operation_id{};
    std::uint64_t intent_id{};
    std::uint64_t expected_revision{};
    std::uint64_t observed_revision{};
    PlatformUiOperationState state{PlatformUiOperationState::Idle};
    PlatformUiResultCode code{PlatformUiResultCode::None};
    PlatformUiPluginMutation mutation{PlatformUiPluginMutation::None};
    std::string subject_id;
    std::string message;
    bool retryable{};
    std::vector<PluginOperationResult> plugins;
};

using OperationResult = PlatformUiOperationResult;

struct PlatformUiSnapshot final {
    std::uint64_t revision{};
    PlatformRuntimeSummary runtime_summary;
    std::vector<InstalledPluginView> installed_plugins;
    NteCompatibilitySnapshot nte_compatibility;
    PlatformDiagnosticsSummary diagnostics;
    RepositoryCoordinatorSnapshot repository;
    std::vector<PlatformUiAffectedSetPreflight> operation_plans;
    std::vector<PlatformUiOperationResult> operation_results;

    [[nodiscard]] const std::vector<InstalledPluginView>& InstalledCatalog() const noexcept {
        return installed_plugins;
    }
    [[nodiscard]] const InstalledPluginView* FindPlugin(std::string_view id) const noexcept;
};

struct PlatformUiIntentValidation final {
    bool accepted{};
    bool duplicate_intent{};
    bool revision_conflict{};
    bool subject_missing{};
    PlatformUiResultCode code{PlatformUiResultCode::InvalidIntent};
    std::string reason;
    std::optional<PlatformUiAffectedSetPreflight> preflight;
};

struct PlatformUiIntentSubmission final {
    bool accepted{};
    std::uint64_t operation_id{};
    PlatformUiResultCode code{PlatformUiResultCode::InvalidIntent};
    std::string reason;
    std::optional<PlatformUiAffectedSetPreflight> preflight;
};

[[nodiscard]] std::string_view ToString(PlatformUiRoute route) noexcept;
[[nodiscard]] std::string_view ToString(PlatformUiPluginState state) noexcept;
[[nodiscard]] std::string_view ToString(PlatformUiResultCode code) noexcept;
[[nodiscard]] std::string_view ToString(PluginOperationReason reason) noexcept;
[[nodiscard]] PlatformUiPluginState ClassifyPluginState(std::string_view state) noexcept;
[[nodiscard]] PlatformUiPluginMutation ResolvePlatformUiMutation(
    const PlatformUiIntent& intent) noexcept;

[[nodiscard]] bool IsMutationIntent(PlatformUiIntentKind kind) noexcept;
[[nodiscard]] bool MatchesPluginSearch(
    const InstalledPluginView& plugin, std::string_view search) noexcept;
[[nodiscard]] bool MatchesPluginFilter(
    const InstalledPluginView& plugin, PlatformUiPluginFilter filter) noexcept;
[[nodiscard]] std::vector<InstalledPluginView> FilterAndSortPlugins(
    const std::vector<InstalledPluginView>& plugins,
    std::string_view search,
    PlatformUiPluginFilter filter,
    PlatformUiPluginSort sort,
    bool include_developer = true);
[[nodiscard]] std::vector<std::string> FilteredPluginIds(
    const std::vector<InstalledPluginView>& plugins,
    std::string_view search,
    PlatformUiPluginFilter filter,
    PlatformUiPluginSort sort,
    bool include_developer = true);

struct PlatformUiPluginFilterCounts final {
    std::size_t all{};
    std::size_t running{};
    std::size_t disabled{};
    std::size_t issues{};

    [[nodiscard]] std::size_t For(PlatformUiPluginFilter filter) const noexcept;
};

[[nodiscard]] PlatformUiPluginFilterCounts CountFilteredPlugins(
    const std::vector<InstalledPluginView>& plugins, std::string_view search,
    bool include_developer = true);
[[nodiscard]] std::string ReconcilePluginSelection(
    const std::vector<InstalledPluginView>& sorted_plugins,
    std::string_view previous_selection);
void ReconcileUiStateSelection(
    const PlatformUiSnapshot& snapshot, PlatformUiState& state,
    bool include_developer = true);

[[nodiscard]] PlatformUiSnapshot BuildPlatformUiSnapshot(
    std::uint64_t revision, std::vector<ue5mem::PluginView> runtime_plugins);
[[nodiscard]] PlatformUiSnapshot BuildPlatformUiSnapshot(
    std::uint64_t revision,
    std::vector<ue5mem::PluginView> runtime_plugins,
    const PluginCatalogSnapshot& catalog,
    const PluginDependencyPlan& dependencies,
    const std::map<std::string, PluginEnablementDecision, std::less<>>& enablement,
    const PluginDisplayNameMap& display_names,
    const PluginDescriptionMap& descriptions);

[[nodiscard]] NteCompatibilitySnapshot BuildNteCompatibilitySnapshot(
    std::optional<BuildFingerprint> fingerprint,
    std::optional<BuildProfile> profile,
    std::optional<ProfileResolutionSnapshot> resolution,
    std::vector<AvailableServiceVersion> services = {});

[[nodiscard]] PlatformUiAffectedSetPreflight PreflightPluginMutation(
    const PlatformUiSnapshot& snapshot, const PlatformUiIntent& intent);
[[nodiscard]] PlatformUiIntentValidation ValidatePlatformUiIntent(
    const PlatformUiSnapshot& snapshot, const PlatformUiIntent& intent,
    const std::unordered_set<std::uint64_t>& submitted_intents = {});

[[nodiscard]] PlatformUiOperationResult MakeSubmittedOperation(
    const PlatformUiIntent& intent, const PlatformUiAffectedSetPreflight* preflight,
    std::uint64_t operation_id, std::uint64_t observed_revision);
void FinalizeOperation(PlatformUiOperationResult& operation);

class PlatformUiModel final {
public:
    PlatformUiModel();
    explicit PlatformUiModel(PlatformUiSnapshot snapshot);

    void Publish(PlatformUiSnapshot snapshot);
    [[nodiscard]] const PlatformUiSnapshot& Snapshot() const noexcept { return snapshot_; }
    [[nodiscard]] const PlatformUiState& State() const noexcept { return state_; }
    [[nodiscard]] PlatformUiState& State() noexcept { return state_; }
    [[nodiscard]] std::vector<InstalledPluginView> VisiblePlugins(
        bool include_developer = true) const;
    [[nodiscard]] PlatformUiPluginFilterCounts FilterCounts(
        bool include_developer = true) const;
    [[nodiscard]] PlatformUiIntent NewIntent(
        PlatformUiIntentKind kind,
        std::string subject_id = {},
        PlatformUiPluginMutation mutation = PlatformUiPluginMutation::None);
    [[nodiscard]] PlatformUiIntentSubmission Submit(PlatformUiIntent intent);
    void ApplyOperationResult(PlatformUiOperationResult result);
    [[nodiscard]] const std::vector<PlatformUiOperationResult>& PendingOperations() const noexcept {
        return pending_operations_;
    }

private:
    PlatformUiSnapshot snapshot_;
    PlatformUiState state_;
    std::uint64_t next_intent_id_{1};
    std::uint64_t next_operation_id_{1};
    std::unordered_set<std::uint64_t> submitted_intents_;
    std::vector<PlatformUiOperationResult> pending_operations_;
};

}  // namespace anomaly
