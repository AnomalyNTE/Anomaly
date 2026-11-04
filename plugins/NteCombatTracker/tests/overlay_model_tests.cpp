#include "overlay_model.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace nte::combat_stats;
using namespace nte::combat_stats::overlay;

void Check(const bool value, const char* message) {
    if (!value) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

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
    Check(damage[0].label == "Alpha" && damage[1].label == "Gamma",
          "equal metrics use character label as deterministic tie breaker");
    const auto crit = SortPlayers(players, SortColumn::CriticalRate, true);
    Check(crit[0].label == "Gamma" &&
              std::abs(PlayerCriticalRate(crit[0]) - 75.0) < 0.01,
          "critical rate is sortable");
    const auto active = SortPlayers(players, SortColumn::ActiveDps, false);
    Check(active[0].label == "Alpha" && active[2].label == "Beta",
          "ascending active DPS is sortable");
}

void LayoutScalesAndCapsRows() {
    const auto normal = CalculateLayout(560.0F, 400.0F, 1.0F, false, 10);
    Check(normal.title_height == 34.0F && normal.header_height == 28.0F &&
              normal.row_height == 24.0F && normal.footer_height == 24.0F,
          "default title and header dimensions match the design");
    Check(normal.visible_rows == 10, "rows are clipped above the fixed footer");
    const auto dpi = CalculateLayout(840.0F, 600.0F, 1.5F, false, 10);
    Check(dpi.visible_rows == 10 && dpi.resize_grip == 15.0F,
          "DPI scaling preserves logical row count and hit targets");
    const auto dpi200 = CalculateLayout(1120.0F, 800.0F, 2.0F, false, 10);
    Check(dpi200.visible_rows == 10 && dpi200.title_height == 68.0F,
          "200 percent DPI preserves logical layout proportions");
    const auto compact = CalculateLayout(520.0F, 220.0F, 1.0F, false, 10);
    Check(compact.visible_rows == 5,
          "compact layout should expose five rows at the persisted reference extent");
    const auto collapsed = CalculateLayout(520.0F, 220.0F, 1.0F, true, 10);
    Check(collapsed.visible_rows == 0 && collapsed.header_height == 0.0F,
          "collapsed layout only exposes the title bar");
}

void ReferenceVisualModelIsDeterministic() {
    const auto theme = OverlayReferenceTheme();
    Check(theme.title_background == 0x303030 && theme.content_background == 0x898989 &&
              theme.accent == 0x00B8D4,
          "reference theme should expose the fixed dark-gray and cyan palette");
    const auto first = StablePlayerColor("path:/Game/Player/player_a_C");
    const auto repeated = StablePlayerColor("path:/Game/Player/player_a_C");
    const auto other = StablePlayerColor("path:/Game/Player/player_b_C");
    Check(first == repeated && first != other,
          "player identity should produce a stable palette color");
    Check(PlayerDisplayColor(0x699A87U, "path:/Game/Player/player_a_C") ==
              0x699A87U,
          "a resolved attribute color should override the identity palette");
    Check(PlayerDisplayColor(std::nullopt, "path:/Game/Player/player_a_C") == first,
          "a missing attribute color should retain the stable identity fallback");
    Check(std::abs(PlayerBarFraction(250, 500) - 0.5) < 0.0001 &&
              PlayerBarFraction(800, 500) == 1.0 && PlayerBarFraction(10, 0) == 0.0,
          "damage bars should be normalized to the highest visible damage");
    const auto& columns = OutgoingSummaryColumns();
    Check(columns.size() == 6 &&
              columns[0].label == L"角色" && columns[1].label == L"总伤害" &&
              columns[2].label == L"DPS" && columns[3].label == L"DPS%" &&
              columns[4].label == L"暴击%" && columns[5].label == L"最高伤害",
          "outgoing summary columns should match the fixed reference order");
    Check(IsOutgoingSummarySort(SortColumn::Damage) &&
              IsOutgoingSummarySort(SortColumn::CriticalRate) &&
              !IsOutgoingSummarySort(SortColumn::HeadRate),
          "restored sorting should be limited to visible summary columns");
    Check(RoundedPercent(33.333) == 33 && RoundedPercent(66.667) == 67,
          "summary percentages should use integer rounding");
    Check(PlayerNameInset(1.0F) == 24.0F && PlayerNameInset(1.5F) == 36.0F,
          "player labels should reserve scaled space for the colored glyph");
    const FooterData footer{3, 1811.4, 24, 2, 900.2};
    Check(FooterSummary(FooterPage::Live, footer) == L"3人｜1811dps" &&
              FooterSummary(FooterPage::HistoryList, footer) == L"24场历史战斗" &&
              FooterSummary(FooterPage::HistoryDetail, footer) == L"2人｜900dps",
          "footer text should use the active page data source");
    const auto title = CalculateTitleBarLayout(560.0F, 1.0F, 34.0F);
    Check(title.history.left == 4.0F && title.history.right == 32.0F &&
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
        Check(scaled_title.history.left >= 0.0F &&
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
    Check(color_error.empty(),
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
    Check(expected.has_value() && live_color == *expected &&
              historical_color == live_color,
        "live and historical rows for one character should share its attribute color");

    PlayerRow unresolved = live;
    unresolved.attribute.clear();
    Check(ResolvePlayerDisplayColor(unresolved, colors) ==
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
    Check(done.front().label == "Alpha" && received.front().label == "Beta",
        "healing rows should sort independently for output and received directions");
    Check(HealingAverage(alpha, HealingDirection::Done) == 150.0 &&
              HealingAverage(beta, HealingDirection::Received) == 250.0,
        "healing averages should use the selected direction's event count");
}

void TextAndFooterLayoutMatchTheOverlayContract() {
    const auto styles = OverlayReferenceTextStyles();
    Check(styles.regular_text.alignment == TextAlignment::Center &&
              styles.small_text.alignment == TextAlignment::Center &&
              styles.bold_text.alignment == TextAlignment::Center &&
              styles.tab_text.alignment == TextAlignment::Center,
          "cell, title, breadcrumb, and tab text should be centered");
    Check(styles.footer_text.alignment == TextAlignment::Trailing,
          "footer encounter summary should be right aligned");
    Check(styles.regular_text.logical_size == 12.0F &&
              styles.small_text.logical_size == 11.0F &&
              styles.bold_text.logical_size == 12.0F &&
              styles.tab_text.logical_size == 12.0F &&
              styles.footer_text.logical_size == 12.0F,
          "compact text styles should match the reference density");
    Check(styles.history_icon_text.alignment == TextAlignment::Center &&
              styles.history_icon_text.logical_size == 18.0F &&
              styles.history_icon_text.font_family == L"Segoe UI Symbol",
          "history icon should use a centered compact symbol style");

    const ViewRect cell{0.0F, 34.0F, 170.0F, 62.0F};
    const auto centered = CalculateCellTextRect(cell, 1.0F, 0.0F);
    Check(centered.left == 4.0F && centered.right == 166.0F &&
              (centered.left + centered.right) / 2.0F == 85.0F,
          "ordinary cells should use symmetric horizontal padding");
    const auto player_name = CalculateCellTextRect(cell, 1.0F, PlayerNameInset(1.0F));
    const auto glyph = CalculatePlayerGlyphRect(cell, 1.0F);
    Check(glyph.left == 2.0F && glyph.right == 22.0F &&
              player_name.left == 28.0F && glyph.right < player_name.left,
          "player glyph and centered player name should not overlap");

    const auto footer = CalculateFooterLayout(
        420.0F, 180.0F, 1.0F, 24.0F, 10.0F, true);
    Check(footer.outgoing_tab.left == 0.0F && footer.outgoing_tab.right == 48.0F &&
              footer.incoming_tab.left == 48.0F && footer.incoming_tab.right == 96.0F &&
              footer.healing_done_tab.left == 96.0F && footer.healing_done_tab.right == 144.0F &&
              footer.healing_received_tab.left == 144.0F && footer.healing_received_tab.right == 192.0F,
          "minimum-width footer should preserve all four tabs");
    Check(footer.summary.left == 204.0F && footer.summary.right == 406.0F &&
              footer.summary.right < footer.resize_grip.left,
          "footer summary should extend to the right safe margin without covering resize");

    for (const float scale : {1.0F, 1.25F, 1.5F}) {
        const auto scaled = CalculateFooterLayout(
            420.0F * scale, 180.0F * scale, scale,
            24.0F * scale, 10.0F * scale, true);
        Check(scaled.healing_received_tab.right <= scaled.summary.left &&
                  scaled.summary.right < scaled.resize_grip.left,
              "scaled footer tabs, summary, and resize grip should not overlap");
    }
}

void ExportAvailabilityRequiresACompletedVisibleEncounter() {
    Check(CanExportEncounter(ViewPage::Live, EncounterState::Completed, false),
        "completed live encounter should be exportable");
    Check(!CanExportEncounter(ViewPage::Live, EncounterState::Active, false) &&
            !CanExportEncounter(ViewPage::Live, EncounterState::Idle, false) &&
            !CanExportEncounter(ViewPage::Live, std::nullopt, false),
        "active, idle, and unavailable live encounters should not export");
    Check(CanExportEncounter(ViewPage::HistoryDetail, std::nullopt, true) &&
            !CanExportEncounter(ViewPage::HistoryDetail, std::nullopt, false) &&
            !CanExportEncounter(ViewPage::HistoryList,
                EncounterState::Completed, true),
        "only a loaded history detail should export outside the live page");
}

void HitTestingRespectsLockAndResizeGrip() {
    const auto layout = CalculateLayout(520.0F, 220.0F, 1.0F, false, 2);
    Check(HitTest(layout, 100.0F, 20.0F, false) == HitRegion::Title,
          "unlocked title bar can move the window");
    Check(HitTest(layout, 100.0F, 55.0F, false) == HitRegion::ColumnHeader,
          "column header is interactive");
    Check(HitTest(layout, 519.0F, 219.0F, false) == HitRegion::ResizeGrip,
          "bottom-right corner can resize the window");
    Check(HitTest(layout, 0.0F, 100.0F, false) == HitRegion::ResizeLeft,
          "left edge should resize the window");
    Check(HitTest(layout, 519.0F, 100.0F, false) == HitRegion::ResizeRight,
          "right edge should resize the window");
    Check(HitTest(layout, 100.0F, 219.0F, false) == HitRegion::ResizeBottom,
          "bottom edge should resize the window");
    Check(HitTest(layout, 0.0F, 0.0F, false) == HitRegion::ResizeTopLeft,
          "top-left corner should take precedence over title dragging");
    Check(HitTest(layout, 100.0F, 20.0F, true) == HitRegion::Client,
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
        Check(ResolveWindowHit(HitRegion::Title, x, y, controls) == HitRegion::Client,
            "title controls must receive client clicks instead of dragging the window");
    }
    Check(ResolveWindowHit(HitRegion::Title, 200.0F, 20.0F, controls) ==
              HitRegion::Title,
        "unused title space remains draggable");
    Check(ResolveWindowHit(HitRegion::ResizeTop, 20.0F, 2.0F, controls) ==
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
    Check(state.drill == DrillLevel::Events && state.scroll_offset == 3,
        "idle and incremental refreshes for the same encounter preserve drill navigation");

    ReconcileLiveNavigation(
        state, "report/encounter-b", players, sources);
    Check(state.drill == DrillLevel::Players && state.player_key.empty() &&
              state.source_key.empty() && state.scroll_offset == 0,
        "a genuine encounter transition returns live navigation to players");

    state.drill = DrillLevel::Sources;
    state.player_key = "/Game/Player/Missing";
    ReconcileLiveNavigation(state, "report/encounter-b", players, {});
    Check(state.drill == DrillLevel::Players,
        "a missing selected player safely returns to the player list");
}

void WindowExtentPreservesLogicalSizeAcrossDpiAndUiScale() {
    const auto normal = ScaleWindowExtent(520, 220, 1.0F, 1.0F);
    Check(normal.width == 520 && normal.height == 220,
          "100 percent DPI uses the requested logical extent");
    const auto dpi150 = ScaleWindowExtent(520, 220, 1.5F, 1.0F);
    Check(dpi150.width == 780 && dpi150.height == 330,
          "150 percent DPI scales the physical window extent");
    const auto dpi200WithUiScale = ScaleWindowExtent(520, 220, 2.0F, 1.25F);
    Check(dpi200WithUiScale.width == 1300 && dpi200WithUiScale.height == 550,
          "DPI and user interface scaling compose without clipping rows");

    const auto restored_logical = UnscaleWindowExtent(780, 330, 1.5F, 1.0F);
    const auto restored_at_200 = ScaleWindowExtent(
        restored_logical.width, restored_logical.height, 2.0F, 1.0F);
    Check(restored_logical.width == 520 && restored_logical.height == 220 &&
              restored_at_200.width == 1040 && restored_at_200.height == 440,
          "persisted logical dimensions restore correctly on a different DPI monitor");
}

}  // namespace

int main() {
    SortsEachSupportedMetricStably();
    LayoutScalesAndCapsRows();
    HitTestingRespectsLockAndResizeGrip();
    TitleControlsTakePrecedenceOverWindowDragging();
    NavigationSurvivesRefreshesWithinTheSameEncounter();
    WindowExtentPreservesLogicalSizeAcrossDpiAndUiScale();
    ReferenceVisualModelIsDeterministic();
    LiveAndHistoricalRowsResolveTheSameAttributeColor();
    HealingRowsSortAndAverageByDirection();
    TextAndFooterLayoutMatchTheOverlayContract();
    ExportAvailabilityRequiresACompletedVisibleEncounter();
    std::cout << "All overlay model tests passed\n";
}
