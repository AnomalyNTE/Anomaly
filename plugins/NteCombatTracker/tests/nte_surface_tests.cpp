// Offline surface fixtures: overlay model and publish policy, history browsing,
// settings, export, the overlay text catalog, the snapshot protocol, and the two
// host-ABI surfaces the plugin depends on (name resolution and UID detection).
//
// One executable per dependency group; the group entry point runs every case so
// a single run reports all broken assertions instead of stopping at the first.
#include "test_support.hpp"

#include "app_settings.hpp"
#include "combat_export.hpp"
#include "damage_log.hpp"
#include "history_reader.hpp"
#include "native_layout.hpp"
#include "overlay_model.hpp"
#include "overlay_protocol.hpp"
#include "overlay_publish_policy.hpp"
#include "overlay_text_catalog.hpp"
#include "sdk_compat.hpp"
#include "uid_detector.hpp"

#include "../../../apps/test_host/plugin_profile_fixture_host.hpp"

#include <anomaly/sdk/anomaly_sdk.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace nte::combat_stats;
using namespace nte::combat_stats::overlay;

using nte::combat_stats::NteOverlaySnapshot;
using nte::combat_stats::overlay::CombatLogReader;
using nte::combat_stats::overlay::DetailDirection;
using nte::combat_stats::overlay::TextCatalog;
using nte::damage_log::DamageLogRepository;
using nte::damage_log::EncounterClose;
using nte::damage_log::EncounterOpen;
using nte::damage_log::LoggedCombatEvent;
using nte::damage_log::ReportClose;
using nte::damage_log::ReportOpen;

PlayerRow Player(std::string label, const std::int64_t damage,
                 const std::uint64_t crits, const std::uint64_t hits,
                 const double active_dps) {
    PlayerRow row;
    row.label = std::move(label);
    row.damage = damage;
    row.critical_hits = crits;
    row.positive_hits = hits;
    row.active_dps = active_dps;
    return row;
}

void SortsEachSupportedMetricStably() {
    const std::vector players{
        Player("Beta", 100, 2, 4, 70.0),
        Player("Alpha", 200, 1, 4, 10.0),
        Player("Gamma", 200, 3, 4, 40.0),
    };
    const auto damage = SortPlayers(players, SortColumn::Damage, true);
    NTE_CHECK(damage[0].label == "Alpha" && damage[1].label == "Gamma",
          "equal metrics use character label as deterministic tie breaker");
    const auto crit = SortPlayers(players, SortColumn::CriticalRate, true);
    NTE_CHECK(crit[0].label == "Gamma" &&
              std::abs(PlayerCriticalRate(crit[0]) - 75.0) < 0.01,
          "critical rate is sortable");
    const auto active = SortPlayers(players, SortColumn::ActiveDps, false);
    NTE_CHECK(active[0].label == "Alpha" && active[2].label == "Beta",
          "ascending active DPS is sortable");
}

void LayoutScalesAndCapsRows() {
    const auto normal = CalculateLayout(560.0F, 400.0F, 1.0F, false, 10);
    NTE_CHECK(normal.title_height == 34.0F && normal.header_height == 28.0F &&
              normal.row_height == 24.0F && normal.footer_height == 24.0F,
          "default title and header dimensions match the design");
    NTE_CHECK(normal.visible_rows == 10, "rows are clipped above the fixed footer");
    const auto dpi = CalculateLayout(840.0F, 600.0F, 1.5F, false, 10);
    NTE_CHECK(dpi.visible_rows == 10 && dpi.resize_grip == 15.0F,
          "DPI scaling preserves logical row count and hit targets");
    const auto dpi200 = CalculateLayout(1120.0F, 800.0F, 2.0F, false, 10);
    NTE_CHECK(dpi200.visible_rows == 10 && dpi200.title_height == 68.0F,
          "200 percent DPI preserves logical layout proportions");
    const auto compact = CalculateLayout(520.0F, 220.0F, 1.0F, false, 10);
    NTE_CHECK(compact.visible_rows == 5,
          "compact layout should expose five rows at the persisted reference extent");
    const auto collapsed = CalculateLayout(520.0F, 220.0F, 1.0F, true, 10);
    NTE_CHECK(collapsed.visible_rows == 0 && collapsed.header_height == 0.0F,
          "collapsed layout only exposes the title bar");
}

void ReferenceVisualModelIsDeterministic() {
    const auto theme = OverlayReferenceTheme();
    NTE_CHECK(theme.title_background == 0x303030 && theme.content_background == 0x898989 &&
              theme.accent == 0x00B8D4,
          "reference theme should expose the fixed dark-gray and cyan palette");
    const auto first = StablePlayerColor("path:/Game/Player/player_a_C");
    const auto repeated = StablePlayerColor("path:/Game/Player/player_a_C");
    const auto other = StablePlayerColor("path:/Game/Player/player_b_C");
    NTE_CHECK(first == repeated && first != other,
          "player identity should produce a stable palette color");
    NTE_CHECK(PlayerDisplayColor(0x699A87U, "path:/Game/Player/player_a_C") ==
              0x699A87U,
          "a resolved attribute color should override the identity palette");
    NTE_CHECK(PlayerDisplayColor(std::nullopt, "path:/Game/Player/player_a_C") == first,
          "a missing attribute color should retain the stable identity fallback");
    NTE_CHECK(std::abs(PlayerBarFraction(250, 500) - 0.5) < 0.0001 &&
              PlayerBarFraction(800, 500) == 1.0 && PlayerBarFraction(10, 0) == 0.0,
          "damage bars should be normalized to the highest visible damage");
    const auto& columns = OutgoingSummaryColumns();
    // Labels come from the catalog now, so the contract to guard is the key
    // order and the sort each column maps to.
    NTE_CHECK(columns.size() == 6 &&
              columns[0].sort == SortColumn::Character &&
              columns[1].sort == SortColumn::Damage &&
              columns[2].sort == SortColumn::EncounterDps &&
              columns[3].sort == SortColumn::Share &&
              columns[4].sort == SortColumn::CriticalRate &&
              columns[5].sort == SortColumn::MaximumHit &&
              columns[0].key == "overlay.col.character",
          "outgoing summary columns should match the fixed reference order");
    NTE_CHECK(IsOutgoingSummarySort(SortColumn::Damage) &&
              IsOutgoingSummarySort(SortColumn::CriticalRate) &&
              !IsOutgoingSummarySort(SortColumn::HeadRate),
          "restored sorting should be limited to visible summary columns");
    NTE_CHECK(RoundedPercent(33.333) == 33 && RoundedPercent(66.667) == 67,
          "summary percentages should use integer rounding");
    NTE_CHECK(PlayerNameInset(1.0F) == 24.0F && PlayerNameInset(1.5F) == 36.0F,
          "player labels should reserve scaled space for the colored glyph");
    const FooterData footer{3, 1811.4, 24, 2, 900.2};
    NTE_CHECK(FooterSummary(FooterPage::Live, footer) == L"1811 dps | 3 players" &&
              FooterSummary(FooterPage::HistoryList, footer) == L"24 encounters" &&
              FooterSummary(FooterPage::HistoryDetail, footer) == L"900 dps | 2 players",
          "footer text should use the active page data source");
    const auto title = CalculateTitleBarLayout(560.0F, 1.0F, 34.0F);
    NTE_CHECK(title.history.left == 4.0F && title.history.right == 32.0F &&
              title.manual_end.left == 34.0F && title.manual_end.right == 70.0F &&
              title.time.left == 72.0F && title.title.left == 124.0F &&
              title.target.left == 216.0F && title.target.right == 508.0F &&
              title.collapse.left == 510.0F && title.collapse.right == 534.0F &&
              title.collapse.top == 0.0F && title.collapse.bottom == 34.0F &&
              title.menu.left == 536.0F && title.menu.right == 558.0F,
          "title controls should use the immutable reference layout");

    for (const float scale : {1.0F, 1.25F, 1.5F}) {
        const float width = 420.0F * scale;
        const auto scaled_title = CalculateTitleBarLayout(
            width, scale, 34.0F * scale);
        NTE_CHECK(scaled_title.history.left >= 0.0F &&
                  scaled_title.history.right <= scaled_title.manual_end.left &&
                  scaled_title.manual_end.right <= scaled_title.time.left &&
                  scaled_title.time.right <= scaled_title.title.left &&
                  scaled_title.title.right <= scaled_title.target.left &&
                  scaled_title.target.right <= scaled_title.collapse.left &&
                  scaled_title.collapse.right <= scaled_title.menu.left &&
                  scaled_title.menu.right <= width &&
                  scaled_title.collapse.top == 0.0F &&
                  scaled_title.collapse.bottom == 34.0F * scale,
              "scaled title controls should stay ordered and vertically centered");
    }
}

void LiveAndHistoricalRowsResolveTheSameAttributeColor() {
    std::string color_error;
    const auto colors = AttributeColorMap::Load(
        std::filesystem::path(NTE_TEST_SOURCE_DIR) / "assets/attribute_colors.json",
        color_error);
    NTE_CHECK(color_error.empty(),
        "repository appearance resources should load for row color resolution");

    // The plugin resolves the element from the live DT_Character row and ships
    // it with the row; the overlay only maps it to a colour.
    PlayerRow live;
    live.object_path = "/Game/NTE/Characters/player_076_shinku_C";
    live.identity_key = "path:/Game/NTE/Characters/player_076_shinku_C";
    live.label = "真红";
    live.attribute = "COSMOS";
    PlayerRow historical = live;
    historical.identity_key = "recorded:historical-row";
    historical.label = "recorded-name";

    const auto live_color = ResolvePlayerDisplayColor(live, colors);
    const auto historical_color = ResolvePlayerDisplayColor(
        historical, colors);
    const auto expected = colors.Find("COSMOS");
    NTE_CHECK(expected.has_value() && live_color == *expected &&
              historical_color == live_color,
        "live and historical rows for one character should share its attribute color");

    PlayerRow unresolved = live;
    unresolved.attribute.clear();
    NTE_CHECK(ResolvePlayerDisplayColor(unresolved, colors) ==
              StablePlayerColor(unresolved.identity_key),
        "a row without a resolved attribute keeps its stable identity colour");
}

void HealingRowsSortAndAverageByDirection() {
    PlayerRow alpha;
    alpha.label = "Alpha";
    alpha.healing_done = 300;
    alpha.healing_done_events = 2;
    alpha.healing_received = 100;
    alpha.healing_received_events = 1;
    PlayerRow beta;
    beta.label = "Beta";
    beta.healing_done = 200;
    beta.healing_done_events = 4;
    beta.healing_received = 500;
    beta.healing_received_events = 2;
    const std::vector players{alpha, beta};
    const auto done = SortHealingPlayers(players, HealingDirection::Done);
    const auto received = SortHealingPlayers(players, HealingDirection::Received);
    NTE_CHECK(done.front().label == "Alpha" && received.front().label == "Beta",
        "healing rows should sort independently for output and received directions");
    NTE_CHECK(HealingAverage(alpha, HealingDirection::Done) == 150.0 &&
              HealingAverage(beta, HealingDirection::Received) == 250.0,
        "healing averages should use the selected direction's event count");
}

void TextAndFooterLayoutMatchTheOverlayContract() {
    const auto styles = OverlayReferenceTextStyles();
    NTE_CHECK(styles.regular_text.alignment == TextAlignment::Center &&
              styles.small_text.alignment == TextAlignment::Center &&
              styles.bold_text.alignment == TextAlignment::Center &&
              styles.tab_text.alignment == TextAlignment::Center,
          "cell, title, breadcrumb, and tab text should be centered");
    NTE_CHECK(styles.footer_text.alignment == TextAlignment::Trailing,
          "footer encounter summary should be right aligned");
    NTE_CHECK(styles.regular_text.logical_size == 12.0F &&
              styles.small_text.logical_size == 11.0F &&
              styles.bold_text.logical_size == 12.0F &&
              styles.tab_text.logical_size == 12.0F &&
              styles.footer_text.logical_size == 12.0F,
          "compact text styles should match the reference density");
    NTE_CHECK(styles.history_icon_text.alignment == TextAlignment::Center &&
              styles.history_icon_text.logical_size == 18.0F &&
              styles.history_icon_text.font_family == L"Segoe UI Symbol",
          "history icon should use a centered compact symbol style");

    const ViewRect cell{0.0F, 34.0F, 170.0F, 62.0F};
    const auto centered = CalculateCellTextRect(cell, 1.0F, 0.0F);
    NTE_CHECK(centered.left == 4.0F && centered.right == 166.0F &&
              (centered.left + centered.right) / 2.0F == 85.0F,
          "ordinary cells should use symmetric horizontal padding");
    const auto player_name = CalculateCellTextRect(cell, 1.0F, PlayerNameInset(1.0F));
    const auto glyph = CalculatePlayerGlyphRect(cell, 1.0F);
    NTE_CHECK(glyph.left == 2.0F && glyph.right == 22.0F &&
              player_name.left == 28.0F && glyph.right < player_name.left,
          "player glyph and centered player name should not overlap");

    const auto footer = CalculateFooterLayout(
        420.0F, 180.0F, 1.0F, 24.0F, 10.0F, true);
    NTE_CHECK(footer.outgoing_tab.left == 0.0F && footer.outgoing_tab.right == 48.0F &&
              footer.incoming_tab.left == 48.0F && footer.incoming_tab.right == 96.0F &&
              footer.healing_done_tab.left == 96.0F && footer.healing_done_tab.right == 144.0F &&
              footer.healing_received_tab.left == 144.0F && footer.healing_received_tab.right == 192.0F,
          "minimum-width footer should preserve all four tabs");
    NTE_CHECK(footer.summary.left == 204.0F && footer.summary.right == 406.0F &&
              footer.summary.right < footer.resize_grip.left,
          "footer summary should extend to the right safe margin without covering resize");

    for (const float scale : {1.0F, 1.25F, 1.5F}) {
        const auto scaled = CalculateFooterLayout(
            420.0F * scale, 180.0F * scale, scale,
            24.0F * scale, 10.0F * scale, true);
        NTE_CHECK(scaled.healing_received_tab.right <= scaled.summary.left &&
                  scaled.summary.right < scaled.resize_grip.left,
              "scaled footer tabs, summary, and resize grip should not overlap");
    }
}

void ExportAvailabilityRequiresACompletedVisibleEncounter() {
    NTE_CHECK(CanExportEncounter(ViewPage::Live, EncounterState::Completed, false),
        "completed live encounter should be exportable");
    NTE_CHECK(!CanExportEncounter(ViewPage::Live, EncounterState::Active, false) &&
            !CanExportEncounter(ViewPage::Live, EncounterState::Idle, false) &&
            !CanExportEncounter(ViewPage::Live, std::nullopt, false),
        "active, idle, and unavailable live encounters should not export");
    NTE_CHECK(CanExportEncounter(ViewPage::HistoryDetail, std::nullopt, true) &&
            !CanExportEncounter(ViewPage::HistoryDetail, std::nullopt, false) &&
            !CanExportEncounter(ViewPage::HistoryList,
                EncounterState::Completed, true),
        "only a loaded history detail should export outside the live page");
}

void HitTestingRespectsLockAndResizeGrip() {
    const auto layout = CalculateLayout(520.0F, 220.0F, 1.0F, false, 2);
    NTE_CHECK(HitTest(layout, 100.0F, 20.0F, false) == HitRegion::Title,
          "unlocked title bar can move the window");
    NTE_CHECK(HitTest(layout, 100.0F, 55.0F, false) == HitRegion::ColumnHeader,
          "column header is interactive");
    NTE_CHECK(HitTest(layout, 519.0F, 219.0F, false) == HitRegion::ResizeGrip,
          "bottom-right corner can resize the window");
    NTE_CHECK(HitTest(layout, 0.0F, 100.0F, false) == HitRegion::ResizeLeft,
          "left edge should resize the window");
    NTE_CHECK(HitTest(layout, 519.0F, 100.0F, false) == HitRegion::ResizeRight,
          "right edge should resize the window");
    NTE_CHECK(HitTest(layout, 100.0F, 219.0F, false) == HitRegion::ResizeBottom,
          "bottom edge should resize the window");
    NTE_CHECK(HitTest(layout, 0.0F, 0.0F, false) == HitRegion::ResizeTopLeft,
          "top-left corner should take precedence over title dragging");
    NTE_CHECK(HitTest(layout, 100.0F, 20.0F, true) == HitRegion::Client,
          "locked overlay does not expose interactive hit regions");
}

void TitleControlsTakePrecedenceOverWindowDragging() {
    const std::array controls{
        ViewRect{8.0F, 8.0F, 36.0F, 32.0F},
        ViewRect{42.0F, 8.0F, 84.0F, 32.0F},
        ViewRect{440.0F, 8.0F, 476.0F, 32.0F},
        ViewRect{480.0F, 8.0F, 516.0F, 32.0F},
    };
    for (const auto& control : controls) {
        const float x = (control.left + control.right) / 2.0F;
        const float y = (control.top + control.bottom) / 2.0F;
        NTE_CHECK(ResolveWindowHit(HitRegion::Title, x, y, controls) == HitRegion::Client,
            "title controls must receive client clicks instead of dragging the window");
    }
    NTE_CHECK(ResolveWindowHit(HitRegion::Title, 200.0F, 20.0F, controls) ==
              HitRegion::Title,
        "unused title space remains draggable");
    NTE_CHECK(ResolveWindowHit(HitRegion::ResizeTop, 20.0F, 2.0F, controls) ==
              HitRegion::ResizeTop,
        "resize edges keep precedence over title controls");
}

void NavigationSurvivesRefreshesWithinTheSameEncounter() {
    NavigationState state;
    state.page = ViewPage::Live;
    state.drill = DrillLevel::Events;
    state.encounter_key = "report/encounter-a";
    state.player_key = "/Game/Player/A";
    state.source_key = "42|/Game/Ability/A";
    state.scroll_offset = 3;
    const std::array players{std::string{"/Game/Player/A"}};
    const std::array sources{std::string{"42|/Game/Ability/A"}};

    ReconcileLiveNavigation(
        state, "report/encounter-a", players, sources);
    NTE_CHECK(state.drill == DrillLevel::Events && state.scroll_offset == 3,
        "idle and incremental refreshes for the same encounter preserve drill navigation");

    ReconcileLiveNavigation(
        state, "report/encounter-b", players, sources);
    NTE_CHECK(state.drill == DrillLevel::Players && state.player_key.empty() &&
              state.source_key.empty() && state.scroll_offset == 0,
        "a genuine encounter transition returns live navigation to players");

    state.drill = DrillLevel::Sources;
    state.player_key = "/Game/Player/Missing";
    ReconcileLiveNavigation(state, "report/encounter-b", players, {});
    NTE_CHECK(state.drill == DrillLevel::Players,
        "a missing selected player safely returns to the player list");
}

void WindowExtentPreservesLogicalSizeAcrossDpiAndUiScale() {
    const auto normal = ScaleWindowExtent(520, 220, 1.0F, 1.0F);
    NTE_CHECK(normal.width == 520 && normal.height == 220,
          "100 percent DPI uses the requested logical extent");
    const auto dpi150 = ScaleWindowExtent(520, 220, 1.5F, 1.0F);
    NTE_CHECK(dpi150.width == 780 && dpi150.height == 330,
          "150 percent DPI scales the physical window extent");
    const auto dpi200WithUiScale = ScaleWindowExtent(520, 220, 2.0F, 1.25F);
    NTE_CHECK(dpi200WithUiScale.width == 1300 && dpi200WithUiScale.height == 550,
          "DPI and user interface scaling compose without clipping rows");

    const auto restored_logical = UnscaleWindowExtent(780, 330, 1.5F, 1.0F);
    const auto restored_at_200 = ScaleWindowExtent(
        restored_logical.width, restored_logical.height, 2.0F, 1.0F);
    NTE_CHECK(restored_logical.width == 520 && restored_logical.height == 220 &&
              restored_at_200.width == 1040 && restored_at_200.height == 440,
          "persisted logical dimensions restore correctly on a different DPI monitor");
}

void IdlePollingIsLimitedToOncePerSecond() {
    OverlayPublishPolicy policy;
    const auto start = OverlayPublishPolicy::TimePoint{};
    NTE_CHECK(policy.ShouldPoll(start), "first server poll is allowed");
    policy.ObserveServer(start, 0);
    NTE_CHECK(!policy.ShouldPoll(start + 999ms), "idle poll is throttled");
    NTE_CHECK(policy.ShouldPoll(start + 1s), "idle poll resumes after one second");
}

void ClientJoinAndStateChangesBypassSnapshotThrottle() {
    OverlayPublishPolicy policy;
    const auto start = OverlayPublishPolicy::TimePoint{};
    policy.ObserveServer(start, 1);
    NTE_CHECK(policy.ShouldPublish(start, 1), "new client gets an immediate snapshot");
    policy.RecordPublished(start, 1);
    NTE_CHECK(!policy.ShouldPublish(start + 100ms, 1), "unchanged snapshot is capped at 4 Hz");
    NTE_CHECK(policy.ShouldPublish(start + 100ms, 2), "state transition publishes immediately");
    policy.RecordPublished(start + 100ms, 2);
    NTE_CHECK(policy.ShouldPublish(start + 350ms, 2), "periodic snapshot resumes after 250 ms");
}

void ForcePublishesStoppingWithoutClients() {
    OverlayPublishPolicy policy;
    const auto start = OverlayPublishPolicy::TimePoint{};
    policy.ObserveServer(start, 0);
    NTE_CHECK(policy.ShouldPoll(start + 10ms, true), "forced stop queries server immediately");
    NTE_CHECK(policy.ShouldPublish(start + 10ms, 7, true),
          "producer-stopping publish is attempted even with no clients");
}

void DisabledOverlaySuppressesPollingUntilReenabled() {
    OverlayPublishPolicy policy;
    const auto start = OverlayPublishPolicy::TimePoint{};
    NTE_CHECK(policy.enabled(), "overlay publishing starts enabled");
    policy.SetEnabled(false);
    NTE_CHECK(!policy.enabled() && !policy.ShouldPoll(start, true),
        "disabled overlay suppresses even forced polling");
    policy.SetEnabled(true);
    NTE_CHECK(policy.enabled() && policy.ShouldPoll(start),
        "reenabling makes the next poll immediately eligible");
}

void FailedLaunchUsesTheToggleAsARetry() {
    NTE_CHECK(nte::combat_stats::ResolveOverlayToggleAction(true, true) ==
            OverlayToggleAction::Retry,
        "an enabled overlay with a launch error should retry");
    NTE_CHECK(nte::combat_stats::ResolveOverlayToggleAction(true, false) ==
            OverlayToggleAction::Disable &&
            nte::combat_stats::ResolveOverlayToggleAction(false, false) ==
                OverlayToggleAction::Enable,
        "the normal toggle should disable or enable the overlay");
}

std::filesystem::path UnknownReports(const std::filesystem::path& root) {
    return root / "accounts" / "_unknown" / "reports";
}

void ClosedLogsCanBeBrowsedAndDrilledDown() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-history-reader-test-" + std::to_string(nonce));
    const std::string report_id = "11111111-2222-4333-8444-555555555555";
    const std::string encounter_id = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    const Handle world{1, 1};
    const Handle player{2, 1};
    const Handle enemy{3, 1};
    {
        DamageLogRepository repository(root);
        std::string error;
        NTE_CHECK(repository.Recover(error), "reader fixture repository should initialize");
        NTE_CHECK(repository.BeginReport(ReportOpen{
            report_id, world, 1'700'000'000'000, "build"}, error),
            "reader fixture report should open");
        NTE_CHECK(repository.BeginEncounter(EncounterOpen{
            encounter_id, 1'700'000'001'000, 1'000'000}, error),
            "reader fixture encounter should open");
        DamageEvent first;
        first.sequence = 10;
        first.tick_sequence = 20;
        first.flags = nte::combat_stats::kDamageFlagCritical;
        first.world = world;
        first.attacker = player;
        first.victim = enemy;
        first.name_id = 77;
        first.final_value = 120;
        first.basic_value = 100;
        first.value = 121;
        first.hit_location = {1.5, 2.5, 3.5};
        first.damage_type = 4;
        first.display_type = 5;
        first.reaction_type = 6;
        first.reaction_display_type = 7;
        first.source_name = "SkillA";
        first.source_display_name = "已记录技能";
        first.source_name_origin = "resource";
        first.name_resource_version = "fixture-v1";
        first.attacker_name = "/Game/Player/player_C";
        first.attacker_display_name = "已记录角色";
        first.attacker_name_origin = "resource";
        first.victim_name = "/Game/Enemy/enemy_C";
        NTE_CHECK(repository.Append(LoggedCombatEvent{
            first, 1'700'000'001'250, 1'250'000, true}, error),
            "first fixture event should append");
        auto second = first;
        second.sequence = 11;
        second.attacker = {player.id, 2};
        second.final_value = 80;
        second.flags = 0;
        second.source_display_name = "SkillA";
        second.source_name_origin = "technical";
        second.attacker_display_name = "player_C";
        second.attacker_name_origin = "technical";
        NTE_CHECK(repository.Append(LoggedCombatEvent{
            second, 1'700'000'001'750, 1'750'000, true}, error),
            "second fixture event should append");
        auto immune = second;
        immune.sequence = 12;
        immune.final_value = 0;
        immune.flags = nte::combat_stats::kDamageFlagCritical;
        NTE_CHECK(repository.Append(LoggedCombatEvent{
            immune, 1'700'000'001'850, 1'850'000, true}, error),
            "zero-damage fixture event should append");
        auto negative = immune;
        negative.sequence = 13;
        negative.final_value = -10;
        NTE_CHECK(repository.Append(LoggedCombatEvent{
            negative, 1'700'000'001'950, 1'950'000, true}, error),
            "negative fixture event should append");
        PlayerRow row;
        row.handle = player;
        row.object_path = first.attacker_name;
        row.label = "player_C";
        row.display_name = "已记录角色";
        row.name_origin = "resource";
        row.name_resource_version = "fixture-v1";
        row.damage = 200;
        row.positive_hits = 99;
        row.critical_hits = 99;
        row.maximum_hit = 120;
        row.maximum_source_id = 77;
        row.maximum_source_name = "SkillA";
        row.damage_share = 100.0;
        row.encounter_dps = 100.0;
        EncounterClose close{encounter_id, "manual_end",
            1'700'000'003'000, 3'000'000, 200, 0, 100.0, 2.0, {}};
        close.confirmed_controlled_players.push_back(row);
        NTE_CHECK(repository.EndEncounter(close, error), "fixture encounter should close");
        NTE_CHECK(repository.CloseReport(ReportClose{
            "plugin_stop", 1'700'000'003'100, false}, error),
            "fixture report should close");
    }

    const auto event_path = UnknownReports(root) / report_id / "events-000001.jsonl";
    {
        std::ifstream input(event_path, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());
        const std::string source = "\"source_path\":\"/Game/Player/player_C\"";
        const std::string target = "\"target_path\":\"/Game/Enemy/enemy_C\"";
        const auto source_at = content.find(source);
        NTE_CHECK(source_at != std::string::npos && content.find(target) != std::string::npos,
            "fixture should contain source and target technical paths");
        if (source_at != std::string::npos) {
            content.replace(source_at, source.size(),
                "\"source_path\":\"RecordedAttacker\"");
        }
        if (const auto target_at = content.find(target);
            target_at != std::string::npos) {
            content.replace(target_at, target.size(),
                "\"target_path\":\"RecordedVictim\"");
        }
        std::ofstream output(event_path, std::ios::binary | std::ios::trunc);
        output << content;
    }
    {
        std::ofstream events(event_path, std::ios::binary | std::ios::app);
        events << "{\"encounter_id\":\"ffffffff-ffff-4fff-8fff-ffffffffffff\"}\n";
    }
    {
        std::ofstream encounters(
            UnknownReports(root) / report_id / "encounters.jsonl",
            std::ios::binary | std::ios::app);
        encounters << "{\"malformed\":true}\n";
    }

    CombatLogReader reader(root);
    std::string error;
    const auto history = reader.History(error);
    NTE_CHECK(!error.empty() && history.size() == 1 &&
            history[0].encounters.size() == 1,
        "valid history should remain visible when one summary row is malformed");
    if (!history.empty()) {
        const auto& encounter = history[0].encounters[0];
        NTE_CHECK(history[0].account == nte::account::Unknown() &&
                  encounter.encounter_id == encounter_id &&
                  encounter.end_reason == "manual_end" &&
                  encounter.outgoing_damage == 200 &&
                  encounter.game_build == "build",
              "history should expose encounter identity and summary");
        NTE_CHECK(encounter.critical_data_status ==
                  nte::combat_stats::CriticalDataStatus::Unavailable,
              "old encounter summaries default critical quality to unavailable");
    }
    const auto collapsed = nte::combat_stats::overlay::FlattenHistoryGroups(
        history, {});
    const auto expanded = nte::combat_stats::overlay::FlattenHistoryGroups(
        history, {"_unknown"});
    NTE_CHECK(collapsed.size() == 1 &&
            collapsed[0].kind ==
                nte::combat_stats::overlay::HistoryListRow::Kind::Account &&
            expanded.size() == 2 && expanded[1].encounter &&
            expanded[1].encounter->encounter_id == encounter_id,
        "history groups should flatten into collapsible account folders");
    const auto detail = reader.Encounter(
        nte::account::Unknown(), report_id, encounter_id, error);
    NTE_CHECK(detail.has_value() && error.empty(), "closed encounter detail should load");
    if (detail) {
        NTE_CHECK(detail->summary.critical_data_status ==
                  nte::combat_stats::CriticalDataStatus::Available,
              "complete unified events should reveal available critical metadata");
        NTE_CHECK(detail->players.size() == 1 && detail->players[0].handle == player,
            "report-local actor ids should resolve back to player handles");
        NTE_CHECK(detail->summary.game_build == "build" &&
                  detail->players[0].display_name == "已记录角色" &&
                  detail->players[0].name_origin == "resource" &&
                  detail->events[0].source_display_name == "已记录技能" &&
                  detail->events[0].source_name_origin == "resource",
            "history should retain report build and recorded display-name priority metadata");
        NTE_CHECK(detail->players.size() == 1 && detail->players[0].positive_hits == 2 &&
                  detail->players[0].critical_hits == 1,
            "history should recompute hit and critical counts from raw events across generations");
        NTE_CHECK(detail->events.size() == 4 && detail->events[0].sequence == 10,
            "all matching raw events should load in sequence order");
        NTE_CHECK(detail->events[0].world == world &&
                  detail->events[0].attacker_actor_id != 0 &&
                  detail->events[0].victim_actor_id != 0 &&
                  detail->events[0].ability_id != 0 &&
                  detail->events[0].player_related &&
                  detail->events[0].hit_location[1] == 2.5 &&
                  detail->events[0].damage_type == 4 &&
                  detail->events[0].display_type == 5 &&
                  detail->events[0].reaction_type == 6 &&
                  detail->events[0].reaction_display_type == 7,
            "history events should retain complete raw metadata for export");
        const auto sources = nte::combat_stats::overlay::GroupEventSources(
            detail->events, player, DetailDirection::Outgoing,
            nte::combat_stats::CombatEventKind::Damage);
        NTE_CHECK(sources.size() == 1 && sources[0].name_id == 77 &&
                  sources[0].amount == 200 && sources[0].count == 2 &&
                  sources[0].maximum == 120,
              "player events should aggregate by damage source");
        const auto hits = nte::combat_stats::overlay::FilterEvents(
            detail->events, player, DetailDirection::Outgoing,
            nte::combat_stats::CombatEventKind::Damage, sources[0]);
        NTE_CHECK(hits.size() == 2 && hits[1].received_utc_ms == 1'700'000'001'750,
            "source selection should expose timestamped individual hits");

        auto renamed_events = detail->events;
        renamed_events[1].source = "SkillB";
        const auto renamed_sources = nte::combat_stats::overlay::GroupEventSources(
            renamed_events, player, DetailDirection::Outgoing,
            nte::combat_stats::CombatEventKind::Damage);
        NTE_CHECK(renamed_sources.size() == 2,
            "the same source id with different names should remain distinct");
        if (renamed_sources.size() == 2) {
            const auto renamed_hits = nte::combat_stats::overlay::FilterEvents(
                renamed_events, player, DetailDirection::Outgoing,
                nte::combat_stats::CombatEventKind::Damage, renamed_sources[0]);
            NTE_CHECK(renamed_hits.size() == 1,
                "source drill-down should retain the selected source name");
        }

        auto renumbered_events = detail->events;
        renumbered_events[1].name_id = 88;
        const auto renumbered_sources = nte::combat_stats::overlay::GroupEventSources(
            renumbered_events, player, DetailDirection::Outgoing,
            nte::combat_stats::CombatEventKind::Damage);
        NTE_CHECK(renumbered_sources.size() == 1 && renumbered_sources[0].amount == 200,
            "the same technical source path should aggregate across source id generations");
        if (renumbered_sources.size() == 1) {
            const auto renumbered_hits = nte::combat_stats::overlay::FilterEvents(
                renumbered_events, player, DetailDirection::Outgoing,
                nte::combat_stats::CombatEventKind::Damage, renumbered_sources[0]);
            NTE_CHECK(renumbered_hits.size() == 2,
                "source drill-down should follow technical identity across generations");
        }

        auto incomplete_players = detail->players;
        incomplete_players[0].positive_hits = 7;
        incomplete_players[0].critical_hits = 4;
        nte::combat_stats::overlay::RecomputePlayerHitCounts(
            detail->events, incomplete_players, false);
        NTE_CHECK(incomplete_players[0].positive_hits == 7 &&
                  incomplete_players[0].critical_hits == 4,
            "incomplete history must retain persisted hit counts instead of using partial events");
        NTE_CHECK(nte::combat_stats::overlay::CriticalDataStatusForEvents(
                  {}, incomplete_players, false) ==
                  nte::combat_stats::CriticalDataStatus::Partial,
            "an incomplete history remains partial even when its event segments are empty");
    }

    {
        std::ofstream events(UnknownReports(root) / report_id /
                "events-000001.jsonl", std::ios::binary | std::ios::app);
        events << "{\"encounter_id\":\"" << encounter_id << "\"}\n";
    }
    const auto export_detail = reader.ExportEncounter(
        nte::account::Unknown(), report_id, encounter_id, error);
    NTE_CHECK(export_detail.has_value() && error.empty(),
        "export encounter should load the persisted final record");
    if (export_detail) {
        NTE_CHECK(export_detail->players[0].positive_hits == 99 &&
                  export_detail->players[0].critical_hits == 99 &&
                  export_detail->summary.critical_data_status ==
                      nte::combat_stats::CriticalDataStatus::Unavailable,
            "export should preserve persisted final player and critical metadata");
        NTE_CHECK(export_detail->events[1].attacker.generation == 2,
            "export should preserve raw event handles without display canonicalization");
        NTE_CHECK(export_detail->events[1].source_display_name == "SkillA" &&
                  export_detail->events[1].source_name_origin == "technical" &&
                  export_detail->events[1].attacker_display_name == "player_C" &&
                  export_detail->events[1].attacker_name_origin == "technical",
            "export should preserve event-level recorded names and origins");
        NTE_CHECK(export_detail->events[0].attacker_name == "RecordedAttacker" &&
                  export_detail->events[0].victim_name == "RecordedVictim",
            "export should not replace event-level recorded technical names");
        NTE_CHECK(export_detail->events.size() == 4 &&
                  export_detail->event_read_warnings == 1,
            "export should retain captured events and report malformed target rows");
    }

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void EventAbilityIdDisambiguatesReusedSourceIds() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-history-source-reuse-test-" + std::to_string(nonce));
    const std::string report_id = "33333333-4444-4555-8666-777777777777";
    const std::string encounter_id = "cccccccc-dddd-4eee-8fff-000000000000";
    const auto report = UnknownReports(root) / report_id;
    std::filesystem::create_directories(report);
    {
        std::ofstream actors(report / "actors.jsonl", std::ios::binary);
        actors << "{\"schema_version\":2,\"actor_id\":1,"
                  "\"handle\":{\"id\":\"2\",\"generation\":\"1\"},"
                  "\"path\":\"Player\"}\n"
                  "{\"schema_version\":2,\"actor_id\":2,"
                  "\"handle\":{\"id\":\"3\",\"generation\":\"1\"},"
                  "\"path\":\"Enemy\"}\n";
    }
    {
        std::ofstream abilities(report / "abilities.jsonl", std::ios::binary);
        abilities << "{\"schema_version\":2,\"ability_id\":1,"
                     "\"source_id\":\"9223372036854775813\","
                     "\"name\":\"GE_Player_Shinku_Melee1_1_Damage\","
                     "\"display_name\":\"普通攻击：特种格斗术\","
                     "\"name_origin\":\"resource\"}\n"
                     "{\"schema_version\":2,\"ability_id\":2,"
                     "\"source_id\":\"9223372036854775813\","
                     "\"name\":\"GE_Player_Sagiri_Melee1_Damage\","
                     "\"display_name\":\"鬼郎丸挥击\","
                     "\"name_origin\":\"resource\"}\n";
    }
    {
        std::ofstream events(report / "events-000001.jsonl", std::ios::binary);
        const auto write_event = [&](const int sequence, const int ability_id,
                                     const char* source) {
            events << "{\"schema_version\":2,\"event_type\":\"damage\","
                   << "\"encounter_id\":\"" << encounter_id << "\","
                   << "\"sequence\":\"" << sequence << "\","
                   << "\"tick_sequence\":\"" << sequence << "\","
                   << "\"flags\":0,\"ability_id\":" << ability_id << ','
                   << "\"name_id\":\"9223372036854775813\","
                   << "\"source\":{\"id\":\"2\",\"generation\":\"1\"},"
                   << "\"target\":{\"id\":\"3\",\"generation\":\"1\"},"
                   << "\"final_value\":\"100\",\"basic_value\":\"0\","
                   << "\"value\":\"0\",\"received_utc_ms\":1,"
                   << "\"received_offset_us\":\"1\",\"event_name\":\""
                   << source << "\",\"source_display_name\":\"" << source
                   << "\",\"event_name_origin\":\"technical\"}\n";
        };
        write_event(1, 1, "GE_Player_Shinku_Melee1_1_Damage");
        write_event(2, 2, "GE_Player_Sagiri_Melee1_Damage");
    }

    CombatLogReader reader(root);
    std::string error;
    const auto events = reader.Events(
        nte::account::Unknown(), report_id, encounter_id, error);
    NTE_CHECK(error.empty() && events.size() == 2,
        "history should load events that reuse a source id");
    if (events.size() == 2) {
        NTE_CHECK(events[0].ability_id == 1 &&
                  events[0].source_display_name == "普通攻击：特种格斗术" &&
                  events[1].ability_id == 2 &&
                  events[1].source_display_name == "鬼郎丸挥击",
            "event ability ids must select the matching localized dictionary row");
    }

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void HealingDrilldownSeparatesDirectionsAndEventKinds() {
    const Handle healer{40, 1};
    const Handle target{41, 1};
    nte::combat_stats::overlay::CombatEventDetail heal;
    heal.kind = nte::combat_stats::CombatEventKind::Heal;
    heal.sequence = 1;
    heal.attacker = healer;
    heal.victim = target;
    heal.name_id = 90;
    heal.source = "HealSkill";
    heal.value = 300;
    heal.final_value = 0;
    auto damage = heal;
    damage.kind = nte::combat_stats::CombatEventKind::Damage;
    damage.sequence = 2;
    damage.final_value = 900;
    const std::vector events{heal, damage};

    const auto done = nte::combat_stats::overlay::GroupEventSources(
        events, healer, DetailDirection::Outgoing,
        nte::combat_stats::CombatEventKind::Heal);
    const auto received = nte::combat_stats::overlay::GroupEventSources(
        events, target, DetailDirection::Incoming,
        nte::combat_stats::CombatEventKind::Heal);
    NTE_CHECK(done.size() == 1 && done.front().amount == 300 &&
              received.size() == 1 && received.front().amount == 300,
        "healing drilldown should support output and received directions without damage");
    if (!done.empty()) {
        const auto rows = nte::combat_stats::overlay::FilterEvents(
            events, healer, DetailDirection::Outgoing,
            nte::combat_stats::CombatEventKind::Heal, done.front());
        NTE_CHECK(rows.size() == 1 && rows.front().kind ==
                  nte::combat_stats::CombatEventKind::Heal,
            "healing source selection should expose only individual heal events");
    }
}

void LegacyV1LogsAreNotReadOrConverted() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-history-v1-rejection-test-" + std::to_string(nonce));
    const auto account = root / "accounts" / "_unknown";
    std::filesystem::create_directories(account);
    {
        std::ofstream catalog(account / "catalog.json", std::ios::binary);
        catalog << "{\"schema_version\":1,\"reports\":[]}";
    }
    CombatLogReader reader(root);
    std::string error;
    const auto history = reader.History(error);
    NTE_CHECK(history.empty() && error.find("schema is not supported") != std::string::npos,
        "legacy v1 logs should be rejected instead of read or converted");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

std::filesystem::path TemporaryRoot(const std::string& name) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        (name + "-" + std::to_string(nonce));
}

void OverlaySettingsRemainCompatibleAndPreserveAllFields() {
    const auto root = TemporaryRoot("nte-overlay-settings-test");
    const auto path = root / "overlay-settings.json";
    std::filesystem::create_directories(root);
    {
        std::ofstream stream(path, std::ios::binary);
        stream << R"({"schema_version":2,"x":321,"y":654,"width":777,"height":444,)"
                  R"("locked":true,"collapsed":false,"topmost":false,)"
                  R"("hide_idle":true,"opacity":0.75,"ui_scale":1.25,)"
                  R"("sort_column":5,"descending":false,"active_tab":1})";
    }

    nte::settings::OverlaySettings settings;
    std::string error;
    NTE_CHECK(nte::settings::LoadOverlaySettings(path, settings, error),
        "existing schema v2 overlay settings should load");
    NTE_CHECK(error.empty() && settings.x == 321 && settings.y == 654 &&
            settings.width == 777 && settings.height == 444 && settings.locked &&
            !settings.topmost && settings.hide_idle && settings.opacity == 0.75F &&
            settings.ui_scale == 1.25F &&
            settings.sort_column == nte::combat_stats::overlay::SortColumn::MaximumHit &&
            !settings.descending && settings.active_tab == nte::settings::DataTab::Incoming,
        "all existing overlay settings should be retained");

    settings.opacity = 0.6F;
    settings.active_tab = nte::settings::DataTab::HealingReceived;
    NTE_CHECK(nte::settings::SaveOverlaySettings(path, settings, error),
        "overlay settings should save atomically");
    nte::settings::OverlaySettings round_trip;
    NTE_CHECK(nte::settings::LoadOverlaySettings(path, round_trip, error) &&
            round_trip.x == 321 && round_trip.width == 777 &&
            round_trip.sort_column == nte::combat_stats::overlay::SortColumn::MaximumHit &&
            round_trip.active_tab == nte::settings::DataTab::HealingReceived &&
            round_trip.opacity == 0.6F,
        "editing an exposed setting should preserve hidden fields");

    auto stale_menu_copy = round_trip;
    stale_menu_copy.opacity = 1.0F;
    auto newer_overlay_copy = round_trip;
    newer_overlay_copy.x = 999;
    newer_overlay_copy.sort_column =
        nte::combat_stats::overlay::SortColumn::RollingDps;
    NTE_CHECK(nte::settings::SaveOverlaySettings(path, newer_overlay_copy, error),
        "overlay should be able to save newer hidden state");
    NTE_CHECK(nte::settings::UpdateOverlayPreferences(
            path, stale_menu_copy, round_trip, error) &&
            round_trip.x == 999 &&
            round_trip.sort_column ==
                nte::combat_stats::overlay::SortColumn::RollingDps &&
            round_trip.opacity == 1.0F,
        "plugin preference updates should merge with the latest hidden fields");
    std::filesystem::remove_all(root);
}

void PluginSettingsUseFallbackAndRoundTripUnicodeLogRoot() {
    const auto root = TemporaryRoot("nte-plugin-settings-test");
    const auto path = root / "plugin-settings.json";
    const auto fallback = root / "fallback";
    std::string error;
    const auto missing = nte::settings::LoadPluginSettings(path, fallback, error);
    NTE_CHECK(missing.log_root == fallback && error.empty(),
        "missing plugin settings should quietly use the default log root");

    std::filesystem::create_directories(root);
    {
        std::ofstream stream(path, std::ios::binary);
        stream << "{\"schema_version\":1,\"log_root\":"
               << std::quoted(nte::settings::PathToUtf8(fallback)) << '}';
    }
    const auto legacy = nte::settings::LoadPluginSettings(path, fallback, error);
    NTE_CHECK(error.empty() && legacy.log_root == fallback && !legacy.developer_mode,
        "schema v1 settings without developer_mode should default it off");

    const auto chosen = root / std::filesystem::path(L"战斗统计");
    nte::settings::PluginSettings expected{chosen};
    NTE_CHECK(nte::settings::SavePluginSettings(path, expected, error),
        "plugin settings should save a Unicode log root");
    const auto loaded = nte::settings::LoadPluginSettings(path, fallback, error);
    NTE_CHECK(error.empty() && loaded.log_root == chosen && !loaded.developer_mode,
        "plugin settings should round trip a Unicode log root and default developer mode off");

    expected.developer_mode = true;
    NTE_CHECK(nte::settings::SavePluginSettings(path, expected, error),
        "developer mode should save with plugin settings");
    const auto developer_loaded =
        nte::settings::LoadPluginSettings(path, fallback, error);
    NTE_CHECK(developer_loaded.log_root == chosen && developer_loaded.developer_mode,
        "developer mode should round trip without changing the log root");

    const auto next = root / std::filesystem::path(L"下一目录");
    auto pending = nte::settings::StagePluginSettings(
        path, nte::settings::PluginSettings{next, std::nullopt, true}, error);
    NTE_CHECK(pending.has_value(), "plugin settings update should stage successfully");
    NTE_CHECK(nte::settings::LoadPluginSettings(path, fallback, error).log_root == chosen,
        "staged plugin settings should not be visible before commit");
    NTE_CHECK(pending && pending->Commit(error) &&
            nte::settings::LoadPluginSettings(path, fallback, error).log_root == next &&
            nte::settings::LoadPluginSettings(path, fallback, error).developer_mode,
        "committing staged plugin settings should atomically publish the new root and preserve developer mode");

    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << "not-json";
    }
    const auto malformed = nte::settings::LoadPluginSettings(path, fallback, error);
    NTE_CHECK(malformed.log_root == fallback && !error.empty(),
        "malformed plugin settings should report an error and use the fallback");
    std::filesystem::remove_all(root);
}

void ExportRootUpdatesPreserveConcurrentLogRootChanges() {
    const auto root = TemporaryRoot("nte-plugin-export-settings-test");
    const auto path = root / "plugin-settings.json";
    const auto fallback = root / "fallback";
    const auto original_logs = root / std::filesystem::path(L"原日志");
    const auto next_logs = root / std::filesystem::path(L"新日志");
    const auto exports = root / std::filesystem::path(L"导出目录");
    std::string error;

    NTE_CHECK(nte::settings::SavePluginSettings(path, {original_logs, std::nullopt, true}, error),
        "legacy settings without export_root should still save");
    auto loaded = nte::settings::LoadPluginSettings(path, fallback, error);
    NTE_CHECK(loaded.log_root == original_logs && !loaded.export_root &&
            loaded.developer_mode,
        "missing export_root should load as unset and preserve developer mode");

    auto pending = nte::settings::StagePluginSettings(path, {next_logs}, error);
    NTE_CHECK(pending.has_value(), "log root change should stage before publication");
    nte::settings::PluginSettings merged;
    NTE_CHECK(nte::settings::UpdatePluginExportRoot(
            path, fallback, exports, merged, error) &&
            merged.log_root == original_logs && merged.export_root == exports &&
            merged.developer_mode,
        "setting export_root should preserve the current log_root and developer mode");
    NTE_CHECK(nte::settings::UpdatePluginLogRoot(
            path, original_logs, merged, error) && merged.export_root == exports &&
            merged.developer_mode,
        "updating log_root in place should preserve export_root and developer mode under the same lock");
    NTE_CHECK(pending && pending->Commit(error),
        "staged log root should commit after a concurrent export update");
    loaded = nte::settings::LoadPluginSettings(path, fallback, error);
    NTE_CHECK(loaded.log_root == next_logs && loaded.export_root == exports &&
            loaded.developer_mode,
        "committing log_root should preserve a newer export_root and developer mode");

    const auto final_logs = root / std::filesystem::path(L"最终日志");
    pending = nte::settings::StagePluginSettings(
        path, {final_logs, exports, true}, error);
    NTE_CHECK(nte::settings::UpdatePluginExportRoot(
            path, fallback, std::nullopt, merged, error) &&
            merged.log_root == next_logs && !merged.export_root,
        "clearing export_root should not change log_root");
    NTE_CHECK(pending && pending->Commit(error),
        "staged log root should commit after export_root is cleared");
    loaded = nte::settings::LoadPluginSettings(path, fallback, error);
    NTE_CHECK(loaded.log_root == final_logs && !loaded.export_root,
        "committing log_root should preserve a newer export_root clear");
    std::filesystem::remove_all(root);
}

void LogRootValidationRejectsRelativePathsAndAcceptsWritableDirectories() {
    std::string error;
    NTE_CHECK(!nte::settings::PrepareLogRoot("relative/logs", error) && !error.empty(),
        "relative log roots should be rejected");

    const auto root = TemporaryRoot("nte-log-root-test") /
        std::filesystem::path(L"可写目录");
    NTE_CHECK(nte::settings::PrepareLogRoot(root, error) && error.empty() &&
            std::filesystem::is_directory(root),
        "an absolute writable log root should be prepared");
    const auto file = root.parent_path() / "not-a-directory";
    {
        std::ofstream stream(file, std::ios::binary);
        stream << "occupied";
    }
    NTE_CHECK(!nte::settings::PrepareLogRoot(file, error) && !error.empty(),
        "a regular file should be rejected as a log root");
    std::filesystem::remove_all(root.parent_path());
}

void HideIdlePolicyAppliesImmediatelyToCurrentState() {
    using nte::combat_stats::EncounterState;
    using nte::combat_stats::overlay::ShouldHideOverlay;
    NTE_CHECK(ShouldHideOverlay(true, EncounterState::Idle),
        "hide-idle should hide an idle overlay immediately");
    NTE_CHECK(!ShouldHideOverlay(true, EncounterState::Active) &&
            !ShouldHideOverlay(false, EncounterState::Idle) &&
            !ShouldHideOverlay(true, std::nullopt),
        "hide-idle should not hide active, disabled, or uninitialized overlays");
}

void ExportContainsEveryUniqueEventInSequenceOrder() {
    nte::combat_stats::overlay::EncounterDetail detail;
    detail.summary.report_id = "11111111-1111-1111-1111-111111111111";
    detail.summary.encounter_id = "22222222-2222-2222-2222-222222222222";
    detail.summary.complete = true;
    for (std::uint64_t sequence = 130; sequence >= 1; --sequence) {
        nte::combat_stats::overlay::CombatEventDetail event;
        event.encounter_id = detail.summary.encounter_id;
        event.sequence = sequence;
        detail.events.push_back(event);
    }
    detail.events.push_back(detail.events.back());

    const auto document = nlohmann::json::parse(
        nte::combat_stats::overlay::SerializeCombatExport(detail, "0.5.0", 1));
    NTE_CHECK(document.at("schema_version") == 3 &&
            document.at("account").at("uid").is_null() &&
            document.at("account").at("source") == "unavailable",
        "unknown account exports should use schema v3 with a null UID");
    const auto& events = document.at("events");
    NTE_CHECK(events.size() == 130,
        "export should include every unique event beyond the overlay 128-event limit");
    NTE_CHECK(events.front().at("sequence") == "1" &&
            events.back().at("sequence") == "130",
        "export events should be sorted by sequence");
}

void ExportWriterUsesGeneratedCollisionSafeFilenames() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-combat-export-test-" + std::to_string(nonce));
    nte::combat_stats::overlay::EncounterDetail detail;
    detail.summary.report_id = "11111111-1111-1111-1111-111111111111";
    detail.summary.encounter_id = "22222222-2222-2222-2222-222222222222";
    detail.summary.started_utc_ms = 1'700'000'000'000;
    detail.summary.complete = true;
    std::filesystem::path first;
    std::filesystem::path second;
    std::string error;
    NTE_CHECK(nte::combat_stats::overlay::WriteCombatExport(
            root, detail, "0.5.0", 1, first, error) && error.empty(),
        "writer should create a JSON export in a missing absolute directory");
    NTE_CHECK(nte::combat_stats::overlay::WriteCombatExport(
            root, detail, "0.5.0", 2, second, error) && error.empty(),
        "writer should avoid overwriting an existing export");
    NTE_CHECK(first != second && first.extension() == ".json" &&
            first.filename().wstring().starts_with(L"NTECombat-") &&
            first.filename().wstring().find(L"22222222") != std::wstring::npos &&
            second.stem().wstring().ends_with(L"-2"),
        "generated names should carry time, encounter id, and collision suffix");
    NTE_CHECK(std::filesystem::is_regular_file(first) &&
            std::filesystem::is_regular_file(second) &&
            !std::filesystem::exists(first.wstring() + L".tmp"),
        "atomic export should publish both files without leaving temporary files");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void CompletedEncounterSerializesAsVersionedNteLogsImportDocument() {
    nte::combat_stats::overlay::EncounterDetail detail;
    detail.summary.report_id = "11111111-1111-1111-1111-111111111111";
    detail.summary.encounter_id = "22222222-2222-2222-2222-222222222222";
    detail.summary.game_build = "2026.08";
    detail.summary.account = *nte::account::FromUidText("216065736008");
    detail.summary.started_utc_ms = 1'700'000'000'000;
    detail.summary.closed_utc_ms = 1'700'000'012'500;
    detail.summary.duration_seconds = 12.5;
    detail.summary.outgoing_damage = 9'007'199'254'740'993LL;
    detail.summary.incoming_damage = 321;
    detail.summary.encounter_dps = 720'575'940'379'279.4;
    detail.summary.outgoing_healing = 450;
    detail.summary.incoming_healing = 200;
    detail.summary.outgoing_heal_events = 2;
    detail.summary.incoming_heal_events = 1;
    detail.summary.encounter_hps = 36.0;
    detail.summary.encounter_received_hps = 16.0;
    detail.summary.event_type_counts = {1, 1, 1, 1};
    detail.summary.end_reason = "inactivity";
    detail.summary.complete = true;
    detail.summary.service_unavailable_intervals.push_back({100, 200});
    detail.summary.missing_sequence_ranges.push_back({7, 9});
    detail.summary.open_missing_sequence_start = 11;
    detail.summary.service_intervals_truncated = true;
    detail.summary.missing_ranges_truncated = true;

    nte::combat_stats::PlayerRow player;
    player.identity_key = "path:/Game/Player_中文";
    player.handle = {9'007'199'254'740'993ULL, 4};
    player.object_path = "/Game/Player_中文";
    player.label = "角色\"甲";
    player.display_name = "角色甲";
    player.damage = 9'007'199'254'740'993LL;
    player.positive_hits = 1;
    player.maximum_hit = 9'007'199'254'740'993LL;
    detail.players.push_back(player);

    nte::combat_stats::overlay::CombatEventDetail event;
    event.encounter_id = detail.summary.encounter_id;
    event.sequence = 9'007'199'254'740'993ULL;
    event.tick_sequence = 8;
    event.flags = nte::combat_stats::kDamageFlagCritical;
    event.world = {10, 11};
    event.attacker_actor_id = 12;
    event.victim_actor_id = 13;
    event.ability_id = 14;
    event.name_id = 9'007'199'254'740'995ULL;
    event.attacker = player.handle;
    event.victim = {15, 16};
    event.player_related = true;
    event.final_value = 99;
    event.basic_value = 100;
    event.value = 101;
    event.hit_location = {1.25, 2.5, 3.75};
    event.damage_type = 17;
    event.display_type = 18;
    event.reaction_type = 19;
    event.reaction_display_type = 20;
    event.received_utc_ms = 1'700'000'001'000;
    event.received_offset_us = 1'234'567;
    event.source = "/Game/Ability/Test";
    event.source_display_name = "测试技能";
    event.attacker_name = player.object_path;
    event.attacker_display_name = player.display_name;
    event.victim_name = "/Game/Enemy/Test";
    event.victim_display_name = "测试敌人";
    detail.events.push_back(event);
    auto heal = event;
    heal.sequence += 1;
    heal.kind = nte::combat_stats::CombatEventKind::Heal;
    detail.events.push_back(heal);
    auto buff_add = event;
    buff_add.sequence += 2;
    buff_add.kind = nte::combat_stats::CombatEventKind::BuffAdd;
    buff_add.duration_seconds = 9.5F;
    buff_add.stack_count = 4;
    detail.events.push_back(buff_add);
    auto buff_remove = buff_add;
    buff_remove.sequence += 1;
    buff_remove.kind = nte::combat_stats::CombatEventKind::BuffRemove;
    detail.events.push_back(buff_remove);

    const std::string json = nte::combat_stats::overlay::SerializeCombatExport(
        detail, "0.5.0", 1'700'000'020'000);

    NTE_CHECK(json.find("\"format\": \"nte-combat-export\"") != std::string::npos,
        "export should identify the stable document format");
    NTE_CHECK(json.find("\"schema_version\": 3") != std::string::npos &&
            json.find("\"metric_version\": 1") != std::string::npos,
        "export should version both schema and metrics");
    NTE_CHECK(json.find("\"9007199254740993\"") != std::string::npos,
        "64-bit integers should be emitted as decimal strings");
    NTE_CHECK(json.find("角色甲") != std::string::npos &&
            json.find("\"hit_location\": [") != std::string::npos &&
            json.find("\"reaction_display_type\": 20") != std::string::npos,
        "export should retain Unicode names and complete event metadata");
    const auto document = nlohmann::json::parse(json);
    NTE_CHECK(document.at("account").at("uid") == "216065736008" &&
            document.at("account").at("source") == "ui-role-id",
        "export should carry the encounter account UID once at document scope");
    NTE_CHECK(document.at("capture").at("healing_capture_status") == "unavailable",
        "export should distinguish unavailable healing capture from zero healing");
    const auto& encounter = document.at("encounter");
    NTE_CHECK(encounter.at("service_unavailable_intervals").at(0)
                .at("start_offset_us") == "100" &&
            encounter.at("missing_sequence_ranges").at(0).at("first") == "7" &&
            encounter.at("open_missing_sequence_start") == "11" &&
            encounter.at("quality").at("missing_ranges_truncated") == true,
        "export should retain capture gaps and truncation quality metadata");
    NTE_CHECK(!json.empty() && json.back() == '\n',
        "pretty-printed export should end with a newline");
    NTE_CHECK(document.at("encounter").at("summary").at("outgoing_healing") == "450" &&
              document.at("encounter").at("summary").at("event_type_counts")
                  .at("buff_remove") == "1" &&
              document.at("events").at(1).at("event_type") == "heal" &&
              document.at("events").at(2).at("event_type") == "buff_add" &&
              document.at("events").at(3).at("event_type") == "buff_remove",
          "schema v3 exports should include healing summaries and every event kind");
}

std::filesystem::path TemporaryRoot() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("nte-overlay-text-test-" + std::to_string(nonce));
    std::filesystem::create_directories(root / "locales");
    return root;
}

void WriteCatalog(const std::filesystem::path& root, const std::string_view locale,
                  const std::string_view body) {
    std::ofstream stream(root / "locales" / (std::string(locale) + ".json"),
        std::ios::binary | std::ios::trunc);
    stream << body;
}

// The overlay process has no Host services, so the shipped catalog is its only
// source of translated copy. A miss must fall back to the English literal.
void CatalogResolvesKeysAndFallsBackOnMisses() {
    const auto root = TemporaryRoot();
    WriteCatalog(root, "zh-CN", R"({"schemaVersion":1,"locale":"zh-CN","messages":{
        "overlay.tab.outgoing":"输出","overlay.state.idle":"等待战斗"}})");
    TextCatalog catalog;
    catalog.Load(root, "zh-CN");
    NTE_CHECK(catalog.loaded() && catalog.size() == 2,
        "the catalog should load every message the overlay ships");
    NTE_CHECK(catalog.Text("overlay.tab.outgoing", "Damage") == L"输出",
        "a present key should resolve to the translated text");
    NTE_CHECK(catalog.Text("overlay.absent.key", "Fallback") == L"Fallback",
        "a missing key should keep the caller's English fallback");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void UnknownLocaleLeavesEveryLookupOnItsFallback() {
    const auto root = TemporaryRoot();
    WriteCatalog(root, "zh-CN", R"({"schemaVersion":1,"locale":"zh-CN","messages":{
        "overlay.tab.outgoing":"输出"}})");
    TextCatalog catalog;
    // Asking for a locale the package does not carry is not an error: the
    // overlay stays usable in English.
    catalog.Load(root, "en-US");
    NTE_CHECK(!catalog.loaded() &&
              catalog.Text("overlay.tab.outgoing", "Damage") == L"Damage",
        "an absent locale should fall back to the English literal");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void MalformedCatalogIsTreatedAsAbsent() {
    const auto root = TemporaryRoot();
    WriteCatalog(root, "zh-CN", "{ this is not json");
    TextCatalog catalog;
    catalog.Load(root, "zh-CN");
    NTE_CHECK(!catalog.loaded() &&
              catalog.Text("overlay.tab.outgoing", "Damage") == L"Damage",
        "a malformed catalog should degrade to the English literal, not throw");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

// The locale arrives over the wire from another process; it must not be able to
// steer the read outside the locales directory.
void LocaleTokenCannotEscapeTheLocalesDirectory() {
    const auto root = TemporaryRoot();
    const auto outside = root / "secret.json";
    std::ofstream(outside, std::ios::binary)
        << R"({"messages":{"overlay.tab.outgoing":"LEAKED"}})";
    TextCatalog catalog;
    catalog.Load(root, "../secret");
    NTE_CHECK(!catalog.loaded() &&
              catalog.Text("overlay.tab.outgoing", "Damage") == L"Damage",
        "a locale containing path separators should be refused outright");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

// The plugin ships the same file, so a real lookup against the checked-in
// catalog guards the keys the overlay actually asks for.
void RepositoryCatalogCoversOverlayKeys() {
    TextCatalog catalog;
    catalog.Load(std::filesystem::path(NTE_TEST_SOURCE_DIR), "zh-CN");
    NTE_CHECK(catalog.loaded() && catalog.size() > 50,
        "the checked-in catalog should load for the overlay");
    NTE_CHECK(catalog.Text("overlay.tab.outgoing", "x") != L"x" &&
              catalog.Text("overlay.col.character", "x") != L"x" &&
              catalog.Text("overlay.menu.exit", "x") != L"x" &&
              catalog.Text("overlay.state.idle", "x") != L"x",
        "the keys the overlay asks for should exist in the shipped catalog");
}

// 快照协议的可拷贝性由类型本身保证；这是编译期契约，不需要运行时用例。
static_assert(std::is_copy_constructible_v<NteOverlaySnapshot>);
static_assert(!std::is_copy_assignable_v<NteOverlaySnapshot>);

NteOverlaySnapshot ExampleSnapshot() {
    NteOverlaySnapshot::Data message;
    message.producer_instance_id = "6ee28b4d-7039-4bc4-b068-1468f280745d";
    message.snapshot_sequence = std::numeric_limits<std::uint64_t>::max();
    message.latest_event_sequence = std::numeric_limits<std::uint64_t>::max() - 1;
    message.sent_utc_ms = 1'725'000'000'123;
    message.capture_available = true;
    message.capture_status = "available";
    message.control_status = "manual_end_completed";
    message.account = *nte::account::FromUidText("216065736008");
    message.report_id = "report-id";
    message.encounter_id = "encounter-id";
    message.primary_target = "训练目标·甲";
    message.websocket_port = 14514;
    message.connected_clients = 1;
    message.published_messages = 22;
    message.transport_dropped_messages = 3;
    message.combat.state = nte::combat_stats::EncounterState::Active;
    message.combat.duration_seconds = 12.5;
    message.combat.outgoing_damage = std::numeric_limits<std::int64_t>::max();
    message.combat.incoming_damage = 45;
    message.combat.outgoing_healing = 600;
    message.combat.incoming_healing = 200;
    message.combat.outgoing_heal_events = 3;
    message.combat.incoming_heal_events = 2;
    message.combat.encounter_hps = 48.0;
    message.combat.encounter_received_hps = 16.0;
    message.combat.outgoing_positive_hits = 2;
    message.combat.encounter_dps = 1234.5;
    message.combat.rolling_dps = 500.25;
    message.combat.hp = 88.0;
    message.combat.max_hp = 100.0;
    message.combat.shield = 9.0;
    message.combat.quality.partial = true;
    message.combat.critical_data_status = nte::combat_stats::CriticalDataStatus::Partial;
    message.combat.damage_flag_histogram = {{0, 1}, {16, 2}};
    nte::combat_stats::PlayerRow player;
    player.identity_key = "path:/Game/角色/player_001_C";
    player.handle = {std::numeric_limits<std::uint64_t>::max(), 7};
    player.object_path = "/Game/角色/player_001_C";
    player.attribute = "INCANTATION";
    player.label = "角色甲";
    player.display_name = "角色甲";
    player.name_origin = "resource";
    player.name_resource_version = "2026.08.28";
    player.damage = 700;
    player.positive_hits = 2;
    player.critical_hits = 1;
    player.head_hits = 1;
    player.weak_hits = 0;
    player.maximum_hit = 500;
    player.maximum_source_id = std::numeric_limits<std::uint64_t>::max() - 2;
    player.maximum_source_name = "终结技\"A\"";
    player.maximum_source_display_name = "终结技甲";
    player.maximum_source_resource_version = "2026.08.28";
    player.damage_share = 70.0;
    player.encounter_dps = 56.0;
    player.active_dps = 70.0;
    player.rolling_dps = 40.0;
    player.incoming_damage = 45;
    player.incoming_positive_hits = 3;
    player.maximum_incoming_hit = 20;
    player.incoming_share = 100.0;
    player.healing_done = 600;
    player.healing_done_events = 3;
    player.maximum_heal = 300;
    player.healing_share = 75.0;
    player.encounter_hps = 48.0;
    player.healing_received = 200;
    player.healing_received_events = 2;
    player.maximum_heal_received = 120;
    player.healing_received_share = 25.0;
    player.healing_received_hps = 16.0;
    message.combat.players.push_back(std::move(player));
    nte::combat_stats::RecentEvent event;
    event.sequence = 99;
    event.tick_sequence = 100;
    event.flags = 0x0f;
    event.direction = nte::combat_stats::EventDirection::Outgoing;
    event.name_id = std::numeric_limits<std::uint64_t>::max() - 3;
    event.attacker = {91, 2};
    event.victim = {92, 3};
    event.final_value = 500;
    event.received_utc_ms = message.sent_utc_ms;
    event.received_offset_us = 2'500'000;
    event.source_name = "Skill";
    event.source_display_name = "技能甲";
    event.attacker_name = "Player";
    event.attacker_display_name = "角色甲";
    event.victim_name = "Enemy";
    message.combat.recent_events.push_back(std::move(event));
    for (const auto kind : {nte::combat_stats::CombatEventKind::Heal,
             nte::combat_stats::CombatEventKind::BuffAdd,
             nte::combat_stats::CombatEventKind::BuffRemove}) {
        auto extra = message.combat.recent_events.front();
        extra.sequence += message.combat.recent_events.size();
        extra.kind = kind;
        extra.duration_seconds = 8.5F;
        extra.stack_count = 2;
        message.combat.recent_events.push_back(std::move(extra));
    }
    message.combat.controlled_character_count = 1;
    return NteOverlaySnapshot(std::move(message));
}

void RoundTripPreservesVersionedNteSnapshot() {
    const auto expected = ExampleSnapshot();
    const std::string json = nte::combat_stats::SerializeOverlaySnapshot(expected);
    NTE_CHECK(json.find("CombatData") == std::string::npos,
          "NTE protocol must not emit ACT CombatData");
    std::string error;
    const auto parsed = nte::combat_stats::ParseOverlaySnapshot(json, error);
    NTE_CHECK(parsed.has_value(), "valid snapshot should parse");
    NTE_CHECK(error.empty(), "valid snapshot should not produce an error");
    if (!parsed) return;
    NTE_CHECK(parsed->snapshot_sequence == expected.snapshot_sequence,
          "uint64 snapshot sequence must retain decimal precision");
    NTE_CHECK(parsed->combat.outgoing_damage == expected.combat.outgoing_damage,
          "int64 damage must retain decimal precision");
    NTE_CHECK(parsed->combat.outgoing_healing == 600 &&
              parsed->combat.incoming_healing == 200 &&
              parsed->combat.encounter_received_hps == 16.0,
          "healing output and received summaries should round-trip");
    NTE_CHECK(parsed->combat.critical_data_status ==
              nte::combat_stats::CriticalDataStatus::Partial &&
              parsed->combat.damage_flag_histogram.size() == 2,
          "critical data quality and raw flag histogram should round-trip");
    NTE_CHECK(parsed->primary_target == expected.primary_target,
          "Unicode target names must round-trip");
    NTE_CHECK(parsed->control_status == "manual_end_completed",
          "optional control status should round-trip");
    NTE_CHECK(parsed->account == expected.account,
          "account identity should round-trip through overlay v1");
    NTE_CHECK(parsed->combat.players.size() == 1,
          "player rows should round-trip");
    if (!parsed->combat.players.empty()) {
        const auto& player = parsed->combat.players.front();
        NTE_CHECK(player.handle.id == std::numeric_limits<std::uint64_t>::max(),
              "generation handle ids must retain precision");
        NTE_CHECK(player.maximum_source_name == "终结技\"A\"",
              "ability names must use correct JSON escaping");
        NTE_CHECK(player.display_name == "角色甲" &&
                  player.maximum_source_display_name == "终结技甲" &&
                  player.name_origin == "resource" &&
                  player.name_resource_version == "2026.08.28",
              "optional localized player and source names should round-trip");
        NTE_CHECK(player.incoming_damage == 45 && player.incoming_positive_hits == 3 &&
                  player.maximum_incoming_hit == 20 && player.incoming_share == 100.0,
              "optional incoming player metrics should round-trip");
        NTE_CHECK(player.attribute == "INCANTATION",
              "the resolved element attribute should round-trip with the row");
        NTE_CHECK(player.healing_done == 600 && player.healing_done_events == 3 &&
                  player.healing_received == 200 &&
                  player.healing_received_events == 2 &&
                  player.healing_received_hps == 16.0,
              "healing output and received metrics should round-trip");
    }
    // 先算成 bool 再断言：宏参数里的 `==` 与花括号初始化会被预处理器当作分隔符。
    const bool recent_identifiers_kept = parsed->combat.recent_events.size() == 4 &&
              parsed->combat.recent_events[0].name_id ==
                  std::numeric_limits<std::uint64_t>::max() - 3 &&
              parsed->combat.recent_events[0].attacker ==
                  nte::combat_stats::Handle{91, 2} &&
              parsed->combat.recent_events[0].source_display_name == "技能甲";
    NTE_CHECK(recent_identifiers_kept,
          "recent event identifiers should round-trip");
    if (parsed->combat.recent_events.size() == 4) {
        NTE_CHECK(parsed->combat.recent_events[1].kind ==
                  nte::combat_stats::CombatEventKind::Heal &&
                  parsed->combat.recent_events[2].kind ==
                  nte::combat_stats::CombatEventKind::BuffAdd &&
                  parsed->combat.recent_events[3].kind ==
                  nte::combat_stats::CombatEventKind::BuffRemove &&
                  parsed->combat.recent_events[2].duration_seconds == 8.5F &&
                  parsed->combat.recent_events[2].stack_count == 2,
              "all unified event kinds and buff snapshots should round-trip");
    }
}

void ParserAcceptsLegacyNameCatalogVersion() {
    std::string json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::string current = "name_resource_version";
    std::size_t position{};
    while ((position = json.find(current, position)) != std::string::npos) {
        json.replace(position, current.size(), "name_catalog_version");
        position += std::string_view("name_catalog_version").size();
    }
    std::string error;
    const auto parsed = nte::combat_stats::ParseOverlaySnapshot(json, error);
    NTE_CHECK(parsed.has_value() && error.empty() &&
              parsed->combat.players[0].name_resource_version == "2026.08.28" &&
              parsed->combat.players[0].maximum_source_resource_version ==
                  "2026.08.28",
        "protocol v1 should continue reading the legacy name catalog version field");
}

void ClosingEncounterStateRoundTrips() {
    nte::combat_stats::NteOverlaySnapshot::Data data;
    data.producer_instance_id = "closing-test";
    data.capture_available = true;
    data.capture_status = "available";
    data.healing_capture_status = "unavailable";
    data.combat.state = nte::combat_stats::EncounterState::Closing;
    const nte::combat_stats::NteOverlaySnapshot snapshot(std::move(data));
    const auto json = nte::combat_stats::SerializeOverlaySnapshot(snapshot);
    NTE_CHECK(json.find("\"state\":\"closing\"") != std::string::npos,
        "closing state should be serialized explicitly");
    NTE_CHECK(json.find("\"healing_capture_status\":\"unavailable\"") !=
              std::string::npos,
        "overlay protocol should disclose unavailable healing capture");
    std::string error;
    const auto parsed = nte::combat_stats::ParseOverlaySnapshot(json, error);
    NTE_CHECK(parsed && parsed->combat.state == nte::combat_stats::EncounterState::Closing,
        "closing state should round-trip through the overlay protocol");
    NTE_CHECK(parsed && parsed->healing_capture_status == "unavailable",
        "healing capture status should round-trip through the overlay protocol");
}

void ParserDefaultsMissingOptionalExtensions() {
    std::string json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const auto incoming = json.find("\"incoming_metrics\"");
    NTE_CHECK(incoming != std::string::npos, "fixture should contain incoming extension");
    if (incoming != std::string::npos) {
        const auto begin = json.rfind(',', incoming);
        const auto end = json.find('}', incoming);
        json.erase(begin, end - begin + 1);
    }
    const auto events = json.find(",\"recent_events\"");
    NTE_CHECK(events != std::string::npos, "fixture should contain recent event extension");
    if (events != std::string::npos) {
        const auto end = json.find(",\"report_id\"", events);
        json.erase(events, end - events);
    }
    const std::string control = ",\"control_status\":\"manual_end_completed\"";
    const auto control_at = json.find(control);
    NTE_CHECK(control_at != std::string::npos, "fixture should contain control status extension");
    if (control_at != std::string::npos) json.erase(control_at, control.size());
    const std::string account =
        "\"account\":{\"source\":\"ui-role-id\",\"uid\":\"216065736008\"},";
    const auto account_at = json.find(account);
    NTE_CHECK(account_at != std::string::npos, "fixture should contain account extension");
    if (account_at != std::string::npos) json.erase(account_at, account.size());
    std::string error;
    const auto parsed = nte::combat_stats::ParseOverlaySnapshot(json, error);
    NTE_CHECK(parsed.has_value(), "snapshots without optional extensions should remain compatible");
    if (parsed) {
        NTE_CHECK(parsed->combat.players[0].incoming_damage == 0 &&
                  parsed->combat.recent_events.empty() && parsed->control_status.empty() &&
                  parsed->account == nte::account::Unknown(),
              "missing optional extensions should use zero and empty defaults");
    }
}

void ParserRejectsWrongProtocolAndAcceptsUnknownFields() {
    std::string json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::size_t insertion = json.find('{') + 1;
    json.insert(insertion, "\"future_field\":{\"ignored\":true},");
    std::string error;
    NTE_CHECK(nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "unknown fields should be ignored");

    json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::string channel = "shikari.nte-combat-tracker.overlay";
    json.replace(json.find(channel), channel.size(), "foreign.channel");
    NTE_CHECK(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "foreign channels must be rejected");
}

void ParserRejectsOversizedMessages() {
    std::string oversized(64U * 1024U + 1U, 'x');
    std::string error;
    NTE_CHECK(!nte::combat_stats::ParseOverlaySnapshot(oversized, error).has_value(),
          "messages over 64 KiB must be rejected");
}

void ParserRejectsForeignSemantics() {
    std::string error;
    std::string json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::string valid_type = "\"message_type\":\"snapshot\"";
    json.replace(json.find(valid_type), valid_type.size(),
        "\"message_type\":\"foreign\"");
    NTE_CHECK(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "unknown message types should be rejected");

    json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::string basis = "\"timestamp_basis\":\"client_receive\"";
    json.replace(json.find(basis), basis.size(),
        "\"timestamp_basis\":\"server\"");
    NTE_CHECK(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "non-client timestamp semantics should be rejected");
}

void ParserRejectsMissingRequiredFieldsAndUnsupportedVersions() {
    std::string error;
    std::string json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::string required = "\"producer_instance_id\":\"6ee28b4d-7039-4bc4-b068-1468f280745d\",";
    json.erase(json.find(required), required.size());
    NTE_CHECK(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "missing required fields should be rejected");

    json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::string schema = "\"schema_version\":2";
    json.replace(json.find(schema), schema.size(), "\"schema_version\":1");
    NTE_CHECK(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "unsupported schema major versions should be rejected");

    json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::string metric = "\"metric_version\":1";
    json.replace(json.find(metric), metric.size(), "\"metric_version\":2");
    NTE_CHECK(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "unsupported metric major versions should be rejected");

    json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::string required_rate = "\"critical_rate\":50.0,";
    json.erase(json.find(required_rate), required_rate.size());
    NTE_CHECK(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "missing required derived player metrics should be rejected");

    json = nte::combat_stats::SerializeOverlaySnapshot(ExampleSnapshot());
    const std::string valid_rate = "\"critical_rate\":50.0";
    json.replace(json.find(valid_rate), valid_rate.size(), "\"critical_rate\":49.0");
    NTE_CHECK(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "inconsistent derived player metrics should be rejected");
}

void DerivedCriticalRateCannotExceedPositiveHits() {
    auto snapshot = ExampleSnapshot();
    std::string json = nte::combat_stats::SerializeOverlaySnapshot(snapshot);
    const std::string hits = "\"positive_hits\":\"2\"";
    json.replace(json.find(hits), hits.size(), "\"positive_hits\":\"0\"");
    std::string error;
    NTE_CHECK(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "critical count with zero positive hits should fail derived-rate validation");

    json = nte::combat_stats::SerializeOverlaySnapshot(snapshot);
    const std::string crits = "\"critical_hits\":\"1\"";
    json.replace(json.find(crits), crits.size(), "\"critical_hits\":\"3\"");
    const std::string rate = "\"critical_rate\":50.0";
    json.replace(json.find(rate), rate.size(), "\"critical_rate\":150.0");
    NTE_CHECK(!nte::combat_stats::ParseOverlaySnapshot(json, error).has_value(),
          "critical hit count greater than positive hits must be rejected explicitly");
}

AnomalyStatusV1 ANOMALY_CALL StatusCombatant(
    void*, AnomalyNteCombatantSnapshotV1*) { return {ANOMALY_STATUS_V1_OK}; }
std::uint64_t ANOMALY_CALL Latest(void*) { return 0; }
AnomalyStatusV1 ANOMALY_CALL Next(
    void*, std::uint64_t, AnomalyNteCombatEventV1*) { return {ANOMALY_STATUS_V1_OK}; }
AnomalyStatusV1 ANOMALY_CALL EventName(
    void*, const AnomalyNteCombatEventV1*, char*, std::size_t*) {
    return {ANOMALY_STATUS_V1_OK};
}
AnomalyStatusV1 ANOMALY_CALL ParticipantName(
    void*, AnomalyGenerationHandleV1, char*, std::size_t*) {
    return {ANOMALY_STATUS_V1_OK};
}

void CombatServiceRequiresTheCompleteV18Tail() {
    AnomalyNteCombatServiceV1 service{};
    service.current_combatant = StatusCombatant;
    service.participant_path_utf8 = ParticipantName;
    service.latest_event_sequence = Latest;
    service.next_event = Next;
    service.event_name_utf8 = EventName;
    service.participant_display_name_utf8 = ParticipantName;

    service.struct_size = offsetof(AnomalyNteCombatServiceV1, latest_event_sequence);
    NTE_CHECK(!nte::sdk_compat::CombatServiceV18Available(&service),
        "a v1.7-sized combat service table must be rejected");
    service.struct_size = sizeof(service);
    NTE_CHECK(nte::sdk_compat::CombatServiceV18Available(&service),
        "a complete v1.8 combat service table should be accepted");
    service.event_name_utf8 = nullptr;
    service.participant_display_name_utf8 = nullptr;
    NTE_CHECK(nte::sdk_compat::CombatServiceV18Available(&service),
        "optional name resolvers must not block unified event capture");
    NTE_CHECK(!nte::sdk_compat::CombatEventNameResolverAvailable(&service) &&
              !nte::sdk_compat::ParticipantDisplayNameResolverAvailable(&service),
        "missing name resolvers should independently fall back to technical names");
}

void EventNamesRequireTheV18ShapeAndNameValidFlag() {
    AnomalyNteCombatServiceV1 service{};
    service.struct_size = sizeof(service);
    service.current_combatant = StatusCombatant;
    service.participant_path_utf8 = ParticipantName;
    service.latest_event_sequence = Latest;
    service.next_event = Next;
    service.event_name_utf8 = EventName;
    service.participant_display_name_utf8 = ParticipantName;
    AnomalyNteCombatEventV1 event{};
    event.struct_size = sizeof(event);
    event.name_id = 42;
    NTE_CHECK(!nte::sdk_compat::CombatEventNameAvailable(&service, &event),
        "event names must not be queried before NAME_VALID is set");
    event.flags = ANOMALY_NTE_COMBAT_EVENT_V1_NAME_VALID;
    NTE_CHECK(nte::sdk_compat::CombatEventNameAvailable(&service, &event),
        "valid v1.8 events with NAME_VALID should allow runtime name resolution");
}

// 验证 NCT 的名称解析完全建立在宿主 ABI 之上，而不是本地字典。
//
// 被测合同来自 docs/api-reference/nte-services.md：
//   - event_name_utf8 / participant_display_name_utf8 返回显示文本；
//   - 名称延后补全，未就绪时返回 NOT_FOUND，调用方应保留记录并重试；
//   - 显示名与 source_name_utf8 / participant_path_utf8 的对象路径不是同一回事。
//
// 下面的伪宿主把「显示名何时可解析」交给用例显式控制，用来模拟宿主的延后补全。

AnomalyStatusV1 Status(const uint32_t code) {
    AnomalyStatusV1 value{};
    value.code = code;
    return value;
}

// 伪宿主：显示名何时可解析由测试显式控制，用来模拟宿主的「延后补全」。
struct FakeHost {
    bool event_display_ready{false};
    bool participant_display_ready{false};
    std::string event_display{"暴风之刃"};
    std::string participant_display{"早雾"};
    std::string event_path{"GE_StormBlade"};
    std::string participant_path{"/Game/Characters/player_003_sagiri"};
};

FakeHost g_host;

AnomalyStatusV1 CopyOut(const std::string& value, char* destination, std::size_t* size) {
    if (size == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    const std::size_t required = value.size() + 1;
    if (destination == nullptr) {
        *size = required;
        return Status(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL);
    }
    if (*size < required) return Status(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL);
    std::memcpy(destination, value.c_str(), required);
    *size = required;
    return Status(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 ANOMALY_CALL FakeEventName(
    void*, const AnomalyNteCombatEventV1*, char* destination, std::size_t* size) {
    if (!g_host.event_display_ready) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
    return CopyOut(g_host.event_display, destination, size);
}

AnomalyStatusV1 ANOMALY_CALL FakeParticipantDisplay(
    void*, AnomalyGenerationHandleV1, char* destination, std::size_t* size) {
    if (!g_host.participant_display_ready) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
    return CopyOut(g_host.participant_display, destination, size);
}

AnomalyStatusV1 ANOMALY_CALL FakeSourceName(
    void*, uint64_t, char* destination, std::size_t* size) {
    return CopyOut(g_host.event_path, destination, size);
}

AnomalyStatusV1 ANOMALY_CALL FakeParticipantPath(
    void*, AnomalyGenerationHandleV1, char* destination, std::size_t* size) {
    return CopyOut(g_host.participant_path, destination, size);
}

// 与插件内一致的两段式查询协议。
template <typename Resolver>
std::string Query(Resolver resolver) {
    std::size_t size{};
    resolver(nullptr, &size);
    if (size <= 1) return {};
    std::string value(size, '\0');
    if (resolver(value.data(), &size).code != ANOMALY_STATUS_V1_OK) return {};
    if (const auto end = value.find('\0'); end != std::string::npos) value.resize(end);
    return value;
}

bool LooksLikeObjectPath(const std::string& value) {
    return value.starts_with("/Game/") || value.starts_with("/Script/");
}

AnomalyNteCombatServiceV1 MakeService() {
    AnomalyNteCombatServiceV1 service{};
    service.struct_size = sizeof(service);
    service.service_version = 1;
    service.event_name_utf8 = &FakeEventName;
    service.participant_display_name_utf8 = &FakeParticipantDisplay;
    service.source_name_utf8 = &FakeSourceName;
    service.participant_path_utf8 = &FakeParticipantPath;
    return service;
}

void EventNameFallsBackToPathThenId() {
    g_host = FakeHost{};
    auto service = MakeService();
    AnomalyNteCombatEventV1 event{};
    event.struct_size = sizeof(event);
    event.name_id = 4242;

    const std::string pending = Query([&](char* d, std::size_t* s) {
        return service.event_name_utf8(service.user, &event, d, s);
    });
    NTE_CHECK(pending.empty(), "a pending event display name must not resolve");

    const std::string path = Query([&](char* d, std::size_t* s) {
        return service.source_name_utf8(service.user, event.name_id, d, s);
    });
    NTE_CHECK(path == "GE_StormBlade", "the technical source name feeds the fallback");

    g_host.event_display_ready = true;
    const std::string display = Query([&](char* d, std::size_t* s) {
        return service.event_name_utf8(service.user, &event, d, s);
    });
    NTE_CHECK(display == "暴风之刃", "the resolved event display name is the localized text");
    NTE_CHECK(display != path, "the display name is not the object path");
}

void ParticipantDisplayWinsOverPath() {
    g_host = FakeHost{};
    auto service = MakeService();
    const AnomalyGenerationHandleV1 handle{7, 3};

    const std::string pending = Query([&](char* d, std::size_t* s) {
        return service.participant_display_name_utf8(service.user, handle, d, s);
    });
    NTE_CHECK(pending.empty(), "a pending participant display name must not resolve");

    const std::string path = Query([&](char* d, std::size_t* s) {
        return service.participant_path_utf8(service.user, handle, d, s);
    });
    NTE_CHECK(LooksLikeObjectPath(path), "the path fallback is an object path");

    g_host.participant_display_ready = true;
    const std::string display = Query([&](char* d, std::size_t* s) {
        return service.participant_display_name_utf8(service.user, handle, d, s);
    });
    NTE_CHECK(display == "早雾", "the resolved participant display name is localized");
    NTE_CHECK(!LooksLikeObjectPath(display), "a real display name is never an object path");
}

void MissingInterfaceDegradesWithoutCrashing() {
    // 旧宿主只填 struct_size 更小的服务表，显示名槽位为空。
    AnomalyNteCombatServiceV1 service{};
    service.struct_size = sizeof(service);
    NTE_CHECK(service.event_name_utf8 == nullptr,
        "an older host leaves the display-name slots empty");
    NTE_CHECK(service.participant_display_name_utf8 == nullptr,
        "an older host leaves the participant display-name slot empty");

    g_host = FakeHost{};
    const std::string none = Query([&](char* d, std::size_t* s) {
        return service.participant_path_utf8 != nullptr
            ? service.participant_path_utf8(service.user,
                  AnomalyGenerationHandleV1{1, 1}, d, s)
            : Status(ANOMALY_STATUS_V1_UNAVAILABLE);
    });
    NTE_CHECK(none.empty(), "without the path interface there is nothing to fall back to");
}

// ---------------------------------------------------------------------------
// Compatibility profile fixtures.
//
// Every signature, game-object offset, vtable slot and record size/stride the
// UID detector uses is declared in profile/compatibility.profile.json. These
// cases load that file through the host package reader and schema validator
// (ProfileHost), so a stale or misparsed profile cannot silently agree with a
// copied constant.
// ---------------------------------------------------------------------------

[[nodiscard]] nte::native::Layout ParseShippedLayout(std::string& error) {
    error.clear();
    anomaly::testing::ProfileHost host("NteCombatTracker");
    anomaly::sdk::ProfileSnapshot profile;
    if (!profile.Load(&host.api, "compatibility.profile.json",
            {"nte", "HTGame.exe", ".text", "nte-combat-tracker-v1"})) {
        error = "the shipped profile contract was rejected";
        return {};
    }
    return nte::native::ParseLayout(profile.Document(), error);
}

[[nodiscard]] const nte::native::Layout& ShippedLayout() {
    static const nte::native::Layout layout = [] {
        std::string error;
        auto parsed = ParseShippedLayout(error);
        if (!error.empty()) std::cerr << "profile fixture: " << error << '\n';
        return parsed;
    }();
    return layout;
}

// Turns a profile pattern ("48 8B 05 ?? ...") into concrete bytes. Wildcards map
// to 0xCC so an injected pattern never matches a run of zero padding.
[[nodiscard]] std::vector<std::uint8_t> PatternBytes(const std::string_view pattern) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(pattern.size() / 3U + 1U);
    for (std::size_t index{}; index + 1 < pattern.size(); index += 3) {
        const std::string_view token = pattern.substr(index, 2);
        if (token == "??") {
            bytes.push_back(0xCC);
            continue;
        }
        unsigned value{};
        const auto parsed = std::from_chars(token.data(), token.data() + 2, value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + 2) return {};
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
    return bytes;
}

void ShippedProfileIsAcceptedByTheHostReader() {
    anomaly::testing::ProfileHost host("NteCombatTracker");
    const auto loaded = anomaly::sdk::LoadProfile(
        anomaly::sdk::Host(&host.api), "compatibility.profile.json");
    NTE_CHECK(static_cast<bool>(loaded), "the shipped profile must load through the SDK reader");
    if (!loaded) {
        std::cerr << "profile load error: " << loaded.error() << '\n';
        return;
    }
    NTE_CHECK(loaded.value().contract == "nte-combat-tracker-v1",
        "the shipped profile must declare the contract the plugin implements");
    anomaly::sdk::ProfileSnapshot profile;
    NTE_CHECK(profile.Load(&host.api, "compatibility.profile.json",
                  {"nte", "HTGame.exe", ".text", "nte-combat-tracker-v1"}),
        "the production compatibility loader must accept the shipped profile");
    std::string error;
    const auto layout = nte::native::ParseLayout(profile.Document(), error);
    NTE_CHECK(error.empty(), error.c_str());
    NTE_CHECK(layout.object_chunk_size != 0 && layout.object_item_stride != 0,
        "the parsed layout must expose a usable object registry shape");
}

void ShippedProfileLayoutMatchesTheCheckedInJson() {
    const auto path = std::filesystem::path(NTE_TEST_SOURCE_DIR) /
        "profile" / "compatibility.profile.json";
    std::ifstream stream(path, std::ios::binary);
    NTE_CHECK(static_cast<bool>(stream), "the shipped profile file must be present");
    if (!stream) return;
    nlohmann::json document;
    try {
        stream >> document;
    } catch (const std::exception&) {
        NTE_CHECK(false, "the shipped profile must be valid JSON");
        return;
    }
    std::string error;
    const auto layout = ParseShippedLayout(error);
    NTE_CHECK(error.empty(), error.c_str());
    if (!error.empty()) return;
    const auto values = document.at("layout");
    const auto value = [&values](const char* key) {
        return values.at(key).get<std::uint32_t>();
    };
    NTE_CHECK(layout.object_name_offset == value("kObjectNameOffset"),
        "the parsed object name offset must match the shipped JSON");
    NTE_CHECK(layout.text_field_offset == value("kTextFieldOffset"),
        "the parsed text field offset must match the shipped JSON");
    NTE_CHECK(layout.set_text_vtable_offset == value("kSetTextVtableOffset"),
        "the parsed vtable slot must match the shipped JSON");
    NTE_CHECK(layout.object_item_stride == value("kObjectItemStride"),
        "the parsed object item stride must match the shipped JSON");
    NTE_CHECK(layout.object_chunk_size == value("kObjectChunkSize"),
        "the parsed object chunk size must match the shipped JSON");
    NTE_CHECK(layout.instanced_struct_data == value("kInstancedStructData"),
        "the parsed FInstancedStruct payload offset must match the shipped JSON");
    NTE_CHECK(layout.row_stride == value("kRowStride"),
        "the parsed DataTable row stride must match the shipped JSON");
    NTE_CHECK(layout.field_next == value("kFieldNext"),
        "the parsed FField next offset must match the shipped JSON");
    NTE_CHECK(layout.diagnostic_slot_step == value("kDiagnosticSlotStep"),
        "the parsed diagnostic slot step must match the shipped JSON");
    NTE_CHECK(layout.gobjects_addend ==
            -static_cast<std::int32_t>(value("kGObjectsAddendMagnitude")),
        "the parsed GObjects addend must be the negated profile magnitude");
}

void ShippedProfileSymbolsResolveAndValidateEntries() {
    anomaly::testing::ProfileHost host("NteCombatTracker");
    const auto loaded = anomaly::sdk::LoadProfile(
        anomaly::sdk::Host(&host.api), "compatibility.profile.json");
    NTE_CHECK(static_cast<bool>(loaded), "the shipped profile must load through the SDK reader");
    if (!loaded) return;
    auto profile = loaded.value();

    // Build a synthetic .text image that contains every declared pattern so the
    // production resolver has a deterministic hit.
    host.image.assign(0x400, std::uint8_t{0x90});
    std::vector<std::pair<std::string, std::uintptr_t>> expected;
    std::size_t cursor = 0x100;
    for (const auto& symbol : profile.symbols) {
        const auto bytes = PatternBytes(symbol.candidates.front().pattern);
        NTE_CHECK(!bytes.empty(), "every declared pattern must parse as hexadecimal bytes");
        if (bytes.empty()) return;
        cursor = (cursor + 0xFU) & ~static_cast<std::size_t>(0xFU);
        if (cursor + bytes.size() > host.image.size()) {
            NTE_CHECK(false, "the synthetic image must hold every declared pattern");
            return;
        }
        std::copy(bytes.begin(), bytes.end(),
            host.image.begin() + static_cast<std::ptrdiff_t>(cursor));
        expected.emplace_back(symbol.name, 0x10000U + cursor);
        cursor += bytes.size() + 0x20U;
    }

    anomaly::sdk::ProfileSnapshot compatibility;
    NTE_CHECK(compatibility.Load(&host.api, "compatibility.profile.json",
                  {"nte", "HTGame.exe", ".text", "nte-combat-tracker-v1"}),
        "the production loader must accept and resolve the shipped profile");
    for (const auto& [name, address] : expected) {
        NTE_CHECK(compatibility.Address(name) == address,
            "each declared profile symbol must resolve to its injected entry point");
    }

    // Entry-byte validation must reject a profile whose recorded entry no longer
    // matches the image instead of using the address blindly.
    auto mutated = profile;
    const auto symbol = std::find_if(mutated.symbols.begin(), mutated.symbols.end(),
        [](const auto& item) { return item.name == "kGObjectsPattern"; });
    NTE_CHECK(symbol != mutated.symbols.end(), "the profile must declare kGObjectsPattern");
    if (symbol == mutated.symbols.end() || symbol->candidates.empty() ||
        symbol->candidates.front().entry.empty()) {
        return;
    }
    symbol->candidates.front().entry.at(0) ^= 0xFF;
    const auto rejected = anomaly::sdk::ResolveProfileSymbol(
        &host.signatures, &host.core, mutated, "kGObjectsPattern");
    NTE_CHECK(!rejected, "a changed entry byte must reject the profile candidate");
}

// --- UID detector fixture ---------------------------------------------------
//
// The fake runtime is built from the shipped profile: object names, the text
// slot, the vtable slot, the item stride and the GObjects accessor all come from
// `nte::native::Layout`. Only the fake function addresses are test-owned wiring.
constexpr std::uint32_t kRoleIdName = 42;
constexpr std::uint32_t kOtherName = 43;

AnomalyStatusV1 Ok() { return {ANOMALY_STATUS_V1_OK, 0, {}}; }
AnomalyStatusV1 Missing() { return {ANOMALY_STATUS_V1_NOT_FOUND, 0, {}}; }

std::string_view View(const AnomalyStringViewV1 value) {
    return {value.data, value.size};
}

struct UnrealString {
    wchar_t* data{};
    std::int32_t count{};
    std::int32_t capacity{};
};

struct UnrealText {
    void* data{};
    std::uint32_t flags{};
    std::uint32_t padding{};
};

struct TextValue {
    std::wstring value;
    std::uint32_t failures{};
};

UnrealString* ANOMALY_CALL TextToString(
    UnrealString* output, const UnrealText* text) {
    auto* value = static_cast<TextValue*>(text->data);
    if (value == nullptr) return nullptr;
    if (value->failures != 0) {
        --value->failures;
        return nullptr;
    }
    output->count = static_cast<std::int32_t>(value->value.size() + 1);
    output->capacity = output->count;
    output->data = new wchar_t[static_cast<std::size_t>(output->count)];
    std::copy(value->value.begin(), value->value.end(), output->data);
    output->data[output->count - 1] = L'\0';
    return output;
}

void ANOMALY_CALL FreeString(void* value) {
    delete[] static_cast<wchar_t*>(value);
}

struct FakeRuntime {
    struct Object {
        std::array<std::byte, 1024> bytes{};
        TextValue text;
        AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(AnomalyUe5ObjectSnapshotV1)};
    };

    FakeRuntime() : layout(ShippedLayout()) {
        host.struct_size = sizeof(host);
        host.host_context = this;
        host.query_service = QueryService;

        objects = {sizeof(objects), ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION,
            this, Generation, Count, SnapshotAt, SnapshotByHandle, FindExact};
        names = {sizeof(names), ANOMALY_UE5_NAMES_SERVICE_V1_VERSION,
            this, ResolveName};

        profile_usable = layout.object_chunk_size != 0 &&
            layout.object_item_stride >= sizeof(std::uintptr_t) &&
            layout.object_item_serial_offset + sizeof(std::uint32_t) <=
                layout.object_item_stride &&
            layout.object_name_offset + sizeof(std::uint32_t) <=
                Object{}.bytes.size() &&
            layout.text_field_offset + sizeof(UnrealText) <=
                Object{}.bytes.size() &&
            layout.set_text_vtable_offset / sizeof(std::uintptr_t) < vtable.size();
        if (!profile_usable) return;

        set_text = 0x123456789ABCULL;
        vtable[layout.set_text_vtable_offset / sizeof(std::uintptr_t)] = set_text;
        bindings.set_text = set_text;
        bindings.free_string = reinterpret_cast<std::uintptr_t>(&FreeString);
        bindings.text_to_string = reinterpret_cast<std::uintptr_t>(&TextToString);
        bindings.gobjects_accessor = reinterpret_cast<std::uintptr_t>(gobjects.data());

        chunks[0] = reinterpret_cast<std::uintptr_t>(items.data());
        *reinterpret_cast<std::uintptr_t*>(
            registry.data() + layout.object_registry_items_offset) =
            reinterpret_cast<std::uintptr_t>(chunks.data());

        const auto accessor = reinterpret_cast<std::uintptr_t>(gobjects.data());
        const auto registry_address = reinterpret_cast<std::uintptr_t>(registry.data());
        const auto displacement = static_cast<std::int32_t>(
            registry_address - (accessor + nte::native::kRipInstructionSize) -
            layout.gobjects_addend);
        std::memcpy(gobjects.data() + nte::native::kRipDisplacementOffset,
            &displacement, sizeof(displacement));
        AddObject(L"template", 0, true);
    }

    Object& AddObject(
        std::wstring text, const std::uint32_t failures = 0,
        const bool is_template = false,
        const std::uint32_t name_id = kRoleIdName) {
        const std::uint32_t index = static_cast<std::uint32_t>(entries.size());
        const std::uint32_t serial = index + 10;
        auto object = std::make_unique<Object>();
        object->text = {std::move(text), failures};
        if (profile_usable &&
            (static_cast<std::size_t>(index) + 1U) * layout.object_item_stride <=
                items.size()) {
            *reinterpret_cast<std::uintptr_t*>(object->bytes.data()) =
                reinterpret_cast<std::uintptr_t>(vtable.data());
            *reinterpret_cast<std::uint32_t*>(
                object->bytes.data() + layout.object_name_offset) = name_id;
            auto* unreal_text = reinterpret_cast<UnrealText*>(
                object->bytes.data() + layout.text_field_offset);
            unreal_text->data = &object->text;
            auto* item = items.data() +
                static_cast<std::size_t>(index) * layout.object_item_stride;
            *reinterpret_cast<std::uintptr_t*>(item) =
                reinterpret_cast<std::uintptr_t>(object->bytes.data());
            *reinterpret_cast<std::uint32_t*>(
                item + layout.object_item_serial_offset) = serial;
        }
        object->snapshot.handle = {
            (static_cast<std::uint64_t>(serial) << 32U) | (index + 1U), generation};
        object->snapshot.name_id = name_id;
        entries.push_back(std::move(object));
        if (is_template) template_handle = entries.back()->snapshot.handle;
        return *entries.back();
    }

    void MakeRoleId(const std::size_t index, std::wstring text) {
        auto& object = *entries.at(index);
        object.text.value = std::move(text);
        object.snapshot.name_id = kRoleIdName;
        if (!profile_usable) return;
        *reinterpret_cast<std::uint32_t*>(
            object.bytes.data() + layout.object_name_offset) = kRoleIdName;
    }

    void ShrinkTo(const std::size_t count) {
        entries.resize(count);
    }

    void ReplaceRoleId(const std::size_t index, std::wstring text,
        const std::uint32_t serial) {
        auto& object = *entries.at(index);
        object.text.value = std::move(text);
        object.snapshot.handle.id = (static_cast<std::uint64_t>(serial) << 32U) |
            static_cast<std::uint64_t>(index + 1U);
        object.snapshot.name_id = kRoleIdName;
        if (!profile_usable) return;
        auto* item = items.data() + index * layout.object_item_stride;
        *reinterpret_cast<std::uint32_t*>(
            item + layout.object_item_serial_offset) = serial;
    }

    void ChangeGeneration(const std::wstring& uid) {
        ++generation;
        entries.clear();
        items.fill(std::byte{});
        AddObject(L"template", 0, true);
        AddObject(uid);
    }

    [[nodiscard]] nte::native::Snapshot snapshot() const {
        nte::native::Snapshot value;
        value.layout = layout;
        value.bindings = bindings;
        value.complete = profile_usable;
        if (!profile_usable) {
            value.error = "the shipped compatibility profile is not usable";
        }
        return value;
    }

    static AnomalyStatusV1 ANOMALY_CALL QueryService(
        void* user, const AnomalyStringViewV1 id, std::uint32_t, const void** service) {
        auto& self = *static_cast<FakeRuntime*>(user);
        if (!self.services_available) return Missing();
        if (View(id) == ANOMALY_UE5_OBJECTS_SERVICE_V1_ID) *service = &self.objects;
        else if (View(id) == ANOMALY_UE5_NAMES_SERVICE_V1_ID) *service = &self.names;
        else return Missing();
        return Ok();
    }

    static std::uint64_t ANOMALY_CALL Generation(void* user) {
        return static_cast<FakeRuntime*>(user)->generation;
    }
    static std::uint32_t ANOMALY_CALL Count(void* user) {
        return static_cast<std::uint32_t>(
            static_cast<FakeRuntime*>(user)->entries.size());
    }
    static AnomalyStatusV1 ANOMALY_CALL SnapshotAt(
        void* user, const std::uint32_t index, AnomalyUe5ObjectSnapshotV1* output) {
        auto& self = *static_cast<FakeRuntime*>(user);
        if (index >= self.entries.size()) return Missing();
        *output = self.entries[index]->snapshot;
        return Ok();
    }
    static AnomalyStatusV1 ANOMALY_CALL SnapshotByHandle(
        void* user, const AnomalyGenerationHandleV1 handle,
        AnomalyUe5ObjectSnapshotV1* output) {
        auto& self = *static_cast<FakeRuntime*>(user);
        for (const auto& entry : self.entries) {
            if (entry->snapshot.handle.id == handle.id &&
                entry->snapshot.handle.generation == handle.generation) {
                *output = entry->snapshot;
                return Ok();
            }
        }
        return Missing();
    }
    static AnomalyStatusV1 ANOMALY_CALL FindExact(
        void* user, AnomalyStringViewV1, AnomalyGenerationHandleV1* output) {
        *output = static_cast<FakeRuntime*>(user)->template_handle;
        return Ok();
    }
    static AnomalyStatusV1 ANOMALY_CALL ResolveName(
        void*, const std::uint32_t id, char* output, std::size_t* size) {
        if (size == nullptr) return Missing();
        const std::string_view name = id == kRoleIdName ?
            "TextBlock_RoleID" : "UnrelatedWidget";
        if (output == nullptr) {
            *size = name.size() + 1;
            return Ok();
        }
        if (*size < name.size() + 1) return Missing();
        std::memcpy(output, name.data(), name.size());
        output[name.size()] = '\0';
        *size = name.size() + 1;
        return Ok();
    }

    const nte::native::Layout& layout;
    nte::native::Bindings bindings{};
    bool profile_usable{};
    AnomalyHostApiV1 host{};
    AnomalyUe5ObjectsServiceV1 objects{};
    AnomalyUe5NamesServiceV1 names{};
    bool services_available{};
    std::uint64_t generation{1};
    std::uintptr_t set_text{};
    std::array<std::uintptr_t, 160> vtable{};
    std::array<std::byte, 16> gobjects{};
    std::array<std::byte, 32> registry{};
    std::array<std::uintptr_t, 1> chunks{};
    // Holds 256 registry items at the profile's declared item stride.
    std::array<std::byte, 65536> items{};
    std::vector<std::unique_ptr<Object>> entries;
    AnomalyGenerationHandleV1 template_handle{};
};

void MissingProfileDisablesNativeFeatures() {
    const auto missing_host = nte::native::Load(nullptr);
    NTE_CHECK(!missing_host.complete && !missing_host.error.empty(),
        "a missing host API must not produce a native snapshot");
    FakeRuntime runtime;
    const auto unavailable = nte::native::Load(&runtime.host);
    NTE_CHECK(!unavailable.complete && !unavailable.error.empty(),
        "a host without profile services must not produce a native snapshot");
    nte::account::UidDetector detector(&runtime.host, unavailable);
    NTE_CHECK(!detector.available(), "an unavailable profile must disable UID detection");
    static_cast<void>(detector.Update());
    const auto diagnostics = detector.diagnostics();
    NTE_CHECK(diagnostics.phase == nte::account::UidDetectorPhase::WaitingForBindings,
        "an unavailable profile must report the waiting-for-bindings phase");
    NTE_CHECK(!diagnostics.missing_bindings.empty(),
        "an unavailable profile must surface its concrete reason");
    NTE_CHECK(!detector.Update().has_value(),
        "an unavailable profile must not publish an account identity");
}

std::optional<nte::account::AccountIdentity> RunUntilDetected(
    nte::account::UidDetector& detector, const std::uint32_t updates = 700) {
    std::optional<nte::account::AccountIdentity> detected;
    for (std::uint32_t index{}; index < updates; ++index) {
        if (const auto value = detector.Update()) detected = value;
    }
    return detected;
}

void DetectorRecoversFromLateServices() {
    FakeRuntime runtime;
    runtime.AddObject(L"UID: 216065736008");
    nte::account::UidDetector detector(&runtime.host, runtime.snapshot());
    static_cast<void>(detector.Update());
    NTE_CHECK(detector.diagnostics().phase ==
            nte::account::UidDetectorPhase::WaitingForServices,
        "missing optional services should be observable, not terminal");

    runtime.services_available = true;
    const auto detected = RunUntilDetected(detector);
    NTE_CHECK(detected && detected->uid == "216065736008" &&
            detector.diagnostics().phase ==
                nte::account::UidDetectorPhase::Detected,
        "the detector should recover when services appear later");
}

void DetectorRetainsTemporarilyUnreadableAndNewestWidgets() {
    FakeRuntime runtime;
    runtime.services_available = true;
    runtime.AddObject(L"UID: 111", 1);
    for (std::uint32_t index{}; index < 65; ++index) {
        runtime.AddObject(L"UID: " + std::to_wstring(1000 + index));
    }
    nte::account::UidDetector detector(&runtime.host, runtime.snapshot());
    const auto detected = RunUntilDetected(detector, 200);
    if (!detected || detected->uid != "1064" ||
        detector.diagnostics().tracked_widgets != 64) {
        std::cerr << "observed uid=" << (detected ? detected->uid : "none")
                  << " widgets=" << detector.diagnostics().tracked_widgets
                  << " failure=" << detector.diagnostics().last_failure << '\n';
    }
    NTE_CHECK(detected && detected->uid == "1064" &&
            detector.diagnostics().tracked_widgets == 64,
        "bounded tracking should prefer the newest live RoleID layers");

    runtime.ChangeGeneration(L"UID: 9002");
    const auto changed = RunUntilDetected(detector, 200);
    NTE_CHECK(changed && changed->uid == "9002",
        "a HUD generation change should rediscover the replacement RoleID widget");
}

void DetectorRetriesTemporarilyUnreadableText() {
    FakeRuntime runtime;
    runtime.services_available = true;
    runtime.AddObject(L"UID: 777", 1);
    nte::account::UidDetector detector(&runtime.host, runtime.snapshot());
    const auto first = RunUntilDetected(detector, 25);
    NTE_CHECK(!first && detector.diagnostics().tracked_widgets == 1 &&
            detector.diagnostics().failed_reads == 1,
        "a temporary text conversion failure should retain the live widget");
    const auto retried = RunUntilDetected(detector, 40);
    NTE_CHECK(retried && retried->uid == "777",
        "a retained widget should be read again after the verification interval");
}

void DetectorRevisitsLateInitializedSlotsAndGrowth() {
    FakeRuntime runtime;
    runtime.services_available = true;
    runtime.AddObject(L"not ready", 0, false, kOtherName);
    nte::account::UidDetector detector(&runtime.host, runtime.snapshot());
    NTE_CHECK(!RunUntilDetected(detector, 40),
        "an unrelated stable slot should not be reported as a UID widget");

    runtime.MakeRoleId(1, L"UID: 4242");
    const auto late = RunUntilDetected(detector, 400);
    NTE_CHECK(late && late->uid == "4242",
        "an empty detector should periodically revisit a late-initialized stable slot");

    runtime.AddObject(L"UID: 5252");
    const auto grown = RunUntilDetected(detector, 80);
    NTE_CHECK(grown && grown->uid == "5252",
        "object table growth should incrementally discover the newest RoleID widget");
}

void DetectorRecoversWhenTheTableShrinksDuringAnIncrementalScan() {
    FakeRuntime runtime;
    runtime.services_available = true;
    runtime.AddObject(L"UID: 111");
    nte::account::UidDetector detector(&runtime.host, runtime.snapshot());
    const auto initial = RunUntilDetected(detector, 30);
    NTE_CHECK(initial && initial->uid == "111",
        "the initial widget should be detected before the shrink scenario");

    for (std::uint32_t index{}; index < 200; ++index) {
        runtime.AddObject(L"other", 0, false, kOtherName);
    }
    static_cast<void>(detector.Update());
    runtime.ShrinkTo(1);
    runtime.AddObject(L"UID: 222");
    const auto recovered = RunUntilDetected(detector, 400);
    NTE_CHECK(recovered && recovered->uid == "222",
        "a mid-scan table shrink should restart a bounded recovery scan without underflow");
}

void InvalidTextDoesNotReplaceAKnownUid() {
    FakeRuntime runtime;
    runtime.services_available = true;
    auto& widget = runtime.AddObject(L"UID: 7001");
    nte::account::UidDetector detector(&runtime.host, runtime.snapshot());
    const auto known = RunUntilDetected(detector, 40);
    NTE_CHECK(known && known->uid == "7001" &&
            detector.diagnostics().current_observation_valid,
        "the initial valid UID should be known and current");

    widget.text.value = L"UID unavailable";
    std::optional<nte::account::AccountIdentity> retained;
    for (std::uint32_t index{}; index < 35; ++index) {
        retained = detector.Update();
    }
    NTE_CHECK(retained && retained->uid == "7001" &&
            !detector.diagnostics().current_observation_valid,
        "invalid text should retain log identity but revoke current observation validity");
    widget.text.value = L"UID: 7002";
    std::optional<nte::account::AccountIdentity> changed;
    for (std::uint32_t index{}; index < 35; ++index) {
        changed = detector.Update();
    }
    NTE_CHECK(changed && changed->uid == "7002" &&
            detector.diagnostics().current_observation_valid,
        "a different valid UID should replace the prior identity and restore validity");
}

void DetectorRecoversFromAStaleReusedHandle() {
    FakeRuntime runtime;
    runtime.services_available = true;
    runtime.AddObject(L"UID: 8101");
    nte::account::UidDetector detector(&runtime.host, runtime.snapshot());
    const auto initial = RunUntilDetected(detector, 40);
    NTE_CHECK(initial && initial->uid == "8101",
        "the original widget should be detected before handle reuse");

    runtime.ReplaceRoleId(1, L"UID: 8102", 500);
    const auto recovered = RunUntilDetected(detector, 100);
    NTE_CHECK(recovered && recovered->uid == "8102",
        "a stale serial/index handle should trigger a nearby recovery scan");
}

}  // namespace

void RunOverlayModel() {
    SortsEachSupportedMetricStably();
    LayoutScalesAndCapsRows();
    ReferenceVisualModelIsDeterministic();
    LiveAndHistoricalRowsResolveTheSameAttributeColor();
    HealingRowsSortAndAverageByDirection();
    TextAndFooterLayoutMatchTheOverlayContract();
    ExportAvailabilityRequiresACompletedVisibleEncounter();
    HitTestingRespectsLockAndResizeGrip();
    TitleControlsTakePrecedenceOverWindowDragging();
    NavigationSurvivesRefreshesWithinTheSameEncounter();
    WindowExtentPreservesLogicalSizeAcrossDpiAndUiScale();
}

void RunOverlayPublishPolicy() {
    IdlePollingIsLimitedToOncePerSecond();
    ClientJoinAndStateChangesBypassSnapshotThrottle();
    ForcePublishesStoppingWithoutClients();
    DisabledOverlaySuppressesPollingUntilReenabled();
    FailedLaunchUsesTheToggleAsARetry();
}

void RunHistoryReader() {
    ClosedLogsCanBeBrowsedAndDrilledDown();
    EventAbilityIdDisambiguatesReusedSourceIds();
    HealingDrilldownSeparatesDirectionsAndEventKinds();
    LegacyV1LogsAreNotReadOrConverted();
}

void RunAppSettings() {
    OverlaySettingsRemainCompatibleAndPreserveAllFields();
    PluginSettingsUseFallbackAndRoundTripUnicodeLogRoot();
    ExportRootUpdatesPreserveConcurrentLogRootChanges();
    LogRootValidationRejectsRelativePathsAndAcceptsWritableDirectories();
    HideIdlePolicyAppliesImmediatelyToCurrentState();
}

void RunCombatExport() {
    ExportContainsEveryUniqueEventInSequenceOrder();
    ExportWriterUsesGeneratedCollisionSafeFilenames();
    CompletedEncounterSerializesAsVersionedNteLogsImportDocument();
}

void RunOverlayTextCatalog() {
    CatalogResolvesKeysAndFallsBackOnMisses();
    UnknownLocaleLeavesEveryLookupOnItsFallback();
    MalformedCatalogIsTreatedAsAbsent();
    LocaleTokenCannotEscapeTheLocalesDirectory();
    RepositoryCatalogCoversOverlayKeys();
}

void RunOverlayProtocol() {
    RoundTripPreservesVersionedNteSnapshot();
    ParserAcceptsLegacyNameCatalogVersion();
    ClosingEncounterStateRoundTrips();
    ParserDefaultsMissingOptionalExtensions();
    ParserRejectsWrongProtocolAndAcceptsUnknownFields();
    ParserRejectsOversizedMessages();
    ParserRejectsForeignSemantics();
    ParserRejectsMissingRequiredFieldsAndUnsupportedVersions();
    DerivedCriticalRateCannotExceedPositiveHits();
}

void RunSdkCompat() {
    CombatServiceRequiresTheCompleteV18Tail();
    EventNamesRequireTheV18ShapeAndNameValidFlag();
}

void RunDisplayNameAbi() {
    EventNameFallsBackToPathThenId();
    ParticipantDisplayWinsOverPath();
    MissingInterfaceDegradesWithoutCrashing();
}

void RunProfile() {
    ShippedProfileIsAcceptedByTheHostReader();
    ShippedProfileLayoutMatchesTheCheckedInJson();
    ShippedProfileSymbolsResolveAndValidateEntries();
    MissingProfileDisablesNativeFeatures();
}

void RunUidDetector() {
    DetectorRecoversFromLateServices();
    DetectorRetainsTemporarilyUnreadableAndNewestWidgets();
    DetectorRetriesTemporarilyUnreadableText();
    DetectorRevisitsLateInitializedSlotsAndGrowth();
    DetectorRecoversWhenTheTableShrinksDuringAnIncrementalScan();
    InvalidTextDoesNotReplaceAKnownUid();
    DetectorRecoversFromAStaleReusedHandle();
}
NTE_TEST_GROUP("nte-surface",
    RunOverlayModel,
    RunOverlayPublishPolicy,
    RunHistoryReader,
    RunAppSettings,
    RunCombatExport,
    RunOverlayTextCatalog,
    RunOverlayProtocol,
    RunSdkCompat,
    RunDisplayNameAbi,
    RunProfile,
    RunUidDetector)
