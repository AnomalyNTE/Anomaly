#include "overlay_model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <ranges>
#include <string_view>

namespace nte::combat_stats::overlay {
namespace {

double Metric(const PlayerRow& row, const SortColumn column) noexcept {
    switch (column) {
    case SortColumn::Character: return 0.0;
    case SortColumn::Damage: return static_cast<double>(row.damage);
    case SortColumn::Share: return row.damage_share;
    case SortColumn::EncounterDps: return row.encounter_dps;
    case SortColumn::CriticalRate: return PlayerCriticalRate(row);
    case SortColumn::MaximumHit: return static_cast<double>(row.maximum_hit);
    case SortColumn::HeadRate: return PlayerHeadRate(row);
    case SortColumn::WeakRate: return PlayerWeakRate(row);
    case SortColumn::ActiveDps: return row.active_dps;
    case SortColumn::RollingDps: return row.rolling_dps;
    }
    return 0.0;
}

}  // namespace

std::vector<PlayerRow> SortPlayers(
    const std::vector<PlayerRow>& players,
    const SortColumn column,
    const bool descending) {
    std::vector<PlayerRow> result = players;
    std::ranges::sort(result, [&](const PlayerRow& left, const PlayerRow& right) {
        if (column == SortColumn::Character) {
            return descending ? left.label > right.label : left.label < right.label;
        }
        const double left_metric = Metric(left, column);
        const double right_metric = Metric(right, column);
        if (std::abs(left_metric - right_metric) > 0.000001) {
            return descending ? left_metric > right_metric : left_metric < right_metric;
        }
        return left.label < right.label;
    });
    return result;
}

std::int64_t HealingAmount(
    const PlayerRow& player, const HealingDirection direction) noexcept {
    return direction == HealingDirection::Done
        ? player.healing_done : player.healing_received;
}

std::uint64_t HealingEventCount(
    const PlayerRow& player, const HealingDirection direction) noexcept {
    return direction == HealingDirection::Done
        ? player.healing_done_events : player.healing_received_events;
}

double HealingAverage(
    const PlayerRow& player, const HealingDirection direction) noexcept {
    const auto count = HealingEventCount(player, direction);
    return count == 0 ? 0.0 :
        static_cast<double>(HealingAmount(player, direction)) /
            static_cast<double>(count);
}

std::vector<PlayerRow> SortHealingPlayers(
    const std::vector<PlayerRow>& players,
    const HealingDirection direction) {
    std::vector<PlayerRow> result = players;
    std::ranges::sort(result, [&](const PlayerRow& left, const PlayerRow& right) {
        const auto left_amount = HealingAmount(left, direction);
        const auto right_amount = HealingAmount(right, direction);
        return left_amount != right_amount
            ? left_amount > right_amount : left.label < right.label;
    });
    return result;
}

LayoutMetrics CalculateLayout(
    const float width,
    const float height,
    const float scale,
    const bool collapsed,
    const std::size_t player_count) noexcept {
    LayoutMetrics result;
    result.width = std::max(0.0F, width);
    result.height = std::max(0.0F, height);
    result.scale = std::clamp(scale, 0.5F, 4.0F);
    const auto reference = OverlayReferenceMetrics();
    result.title_height = reference.title_height * result.scale;
    result.resize_grip = reference.resize_grip * result.scale;
    if (collapsed) return result;
    result.header_height = reference.header_height * result.scale;
    result.row_height = reference.row_height * result.scale;
    result.footer_height = reference.footer_height * result.scale;
    const float available = std::max(
        0.0F, result.height - result.title_height - result.header_height -
            result.footer_height);
    const auto rows = static_cast<std::size_t>(available / result.row_height);
    result.visible_rows = std::min(rows, player_count);
    return result;
}

ReferenceTheme OverlayReferenceTheme() noexcept {
    return {
        0x303030, 0x454245, 0x898989, 0x303030,
        0xF0F0F0, 0xB7B7B7, 0x00B8D4, 0x5D5D5D,
    };
}

ReferenceMetrics OverlayReferenceMetrics() noexcept {
    return {
        34.0F, 28.0F, 24.0F, 24.0F, 10.0F, 24.0F,
        4.0F, 2.0F, 22.0F, 18.0F, 48.0F, 4.0F, 4.0F,
    };
}

OverlayTextStyles OverlayReferenceTextStyles() noexcept {
    const auto metrics = OverlayReferenceMetrics();
    constexpr std::wstring_view ui_font = L"Microsoft YaHei UI";
    return {
        {ui_font, 12.0F, TextAlignment::Center},
        {ui_font, 11.0F, TextAlignment::Center},
        {ui_font, 12.0F, TextAlignment::Center},
        {ui_font, 12.0F, TextAlignment::Center},
        {ui_font, 12.0F, TextAlignment::Trailing},
        {L"Segoe UI Symbol", metrics.history_icon_font_size, TextAlignment::Center},
    };
}

bool ShouldHideOverlay(
    const bool hide_idle,
    const std::optional<EncounterState> state) noexcept {
    return hide_idle && state == EncounterState::Idle;
}

bool CanExportEncounter(
    const ViewPage page,
    const std::optional<EncounterState> live_state,
    const bool history_detail_ready) noexcept {
    if (page == ViewPage::Live) {
        return live_state == EncounterState::Completed;
    }
    return page == ViewPage::HistoryDetail && history_detail_ready;
}

TitleBarLayout CalculateTitleBarLayout(
    const float width,
    const float scale,
    const float title_height) noexcept {
    const float safe_scale = std::clamp(scale, 0.5F, 4.0F);
    const float safe_width = std::max(0.0F, width);
    const float safe_height = std::max(0.0F, title_height);
    return {
        {4 * safe_scale, 4 * safe_scale, 32 * safe_scale,
            safe_height - 4 * safe_scale},
        {34 * safe_scale, 5 * safe_scale, 70 * safe_scale,
            safe_height - 5 * safe_scale},
        {72 * safe_scale, 0, 124 * safe_scale, safe_height},
        {124 * safe_scale, 0, 216 * safe_scale, safe_height},
        {216 * safe_scale, 0, safe_width - 52 * safe_scale, safe_height},
        {safe_width - 50 * safe_scale, 0,
            safe_width - 26 * safe_scale, safe_height},
        {safe_width - 24 * safe_scale, 0,
            safe_width - 2 * safe_scale, safe_height},
    };
}

ViewRect CalculateCellTextRect(
    const ViewRect cell,
    const float scale,
    const float leading_content_inset) noexcept {
    const auto metrics = OverlayReferenceMetrics();
    const float safe_scale = std::clamp(scale, 0.5F, 4.0F);
    const float padding = metrics.cell_horizontal_padding * safe_scale;
    const float left = cell.left + padding + std::max(0.0F, leading_content_inset);
    const float right = std::max(left, cell.right - padding);
    return {left, cell.top, right, cell.bottom};
}

ViewRect CalculatePlayerGlyphRect(
    const ViewRect cell,
    const float scale) noexcept {
    const auto metrics = OverlayReferenceMetrics();
    const float safe_scale = std::clamp(scale, 0.5F, 4.0F);
    return {
        cell.left + metrics.player_glyph_left * safe_scale,
        cell.top,
        cell.left + metrics.player_glyph_right * safe_scale,
        cell.bottom,
    };
}

FooterLayout CalculateFooterLayout(
    const float width,
    const float height,
    const float scale,
    const float footer_height,
    const float resize_grip,
    const bool show_tabs) noexcept {
    const auto metrics = OverlayReferenceMetrics();
    const float safe_scale = std::clamp(scale, 0.5F, 4.0F);
    const float safe_width = std::max(0.0F, width);
    const float safe_height = std::max(0.0F, height);
    const float safe_footer_height = std::max(0.0F, footer_height);
    const float safe_resize_grip = std::max(0.0F, resize_grip);
    const float top = std::max(0.0F, safe_height - safe_footer_height);
    const float tab_width = metrics.footer_tab_width * safe_scale;
    const float summary_left = show_tabs
        ? 4.0F * tab_width + 3.0F * metrics.footer_content_gap * safe_scale
        : 2.0F * metrics.footer_safe_padding * safe_scale;
    const float summary_right = std::max(summary_left,
        safe_width - safe_resize_grip - metrics.footer_safe_padding * safe_scale);
    return {
        show_tabs ? ViewRect{0.0F, top, tab_width, safe_height} : ViewRect{},
        show_tabs ? ViewRect{tab_width, top, 2.0F * tab_width, safe_height} : ViewRect{},
        show_tabs ? ViewRect{2.0F * tab_width, top, 3.0F * tab_width, safe_height} : ViewRect{},
        show_tabs ? ViewRect{3.0F * tab_width, top, 4.0F * tab_width, safe_height} : ViewRect{},
        {summary_left, top, summary_right, safe_height},
        {std::max(0.0F, safe_width - safe_resize_grip),
            std::max(0.0F, safe_height - safe_resize_grip),
            safe_width, safe_height},
    };
}

std::uint32_t StablePlayerColor(const std::string_view identity) noexcept {
    constexpr std::array palette{
        0xC48EA1U, 0xC5A654U, 0xB69A48U, 0x8C4266U,
        0x5D667BU, 0x68769DU, 0x56728AU, 0x777777U,
    };
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char value : identity) {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    return palette[hash % palette.size()];
}

std::uint32_t PlayerDisplayColor(
    const std::optional<std::uint32_t> attribute_color,
    const std::string_view identity) noexcept {
    return attribute_color.value_or(StablePlayerColor(identity));
}

std::uint32_t ResolvePlayerDisplayColor(
    const PlayerRow& player,
    const AttributeColorMap& colors) {
    const auto identity = player.identity_key.empty()
        ? std::string_view(player.label)
        : std::string_view(player.identity_key);
    // The plugin resolves the attribute from the live DT_Character row and
    // ships it with the row; this process only maps it to a colour.
    return PlayerDisplayColor(colors.Find(player.attribute), identity);
}

double PlayerBarFraction(
    const std::int64_t damage,
    const std::int64_t maximum_damage) noexcept {
    if (damage <= 0 || maximum_damage <= 0) return 0.0;
    return std::clamp(static_cast<double>(damage) /
        static_cast<double>(maximum_damage), 0.0, 1.0);
}

const std::array<SummaryColumn, 6>& OutgoingSummaryColumns() noexcept {
    static constexpr std::array<SummaryColumn, 6> columns{{
        {"overlay.col.character", L"Character", SortColumn::Character, 1.7F},
        {"overlay.col.total_damage", L"Total damage", SortColumn::Damage, 1.05F},
        {"overlay.col.dps", L"DPS", SortColumn::EncounterDps, 0.85F},
        {"overlay.col.dps_share", L"DPS%", SortColumn::Share, 0.8F},
        {"overlay.col.critical_rate", L"Crit%", SortColumn::CriticalRate, 0.8F},
        {"overlay.col.max_hit", L"Max hit", SortColumn::MaximumHit, 1.7F},
    }};
    return columns;
}

bool IsOutgoingSummarySort(const SortColumn column) noexcept {
    const auto& columns = OutgoingSummaryColumns();
    return std::ranges::any_of(columns,
        [column](const auto& value) { return value.sort == column; });
}

long long RoundedPercent(const double value) noexcept {
    if (!std::isfinite(value)) return 0;
    return std::llround(std::clamp(value, 0.0, 100.0));
}

float PlayerNameInset(const float scale) noexcept {
    return OverlayReferenceMetrics().player_name_inset *
        std::clamp(scale, 0.5F, 4.0F);
}

std::wstring FooterSummary(const FooterPage page, const FooterData& data) {
    switch (page) {
    // Numeric summary only: the caller localizes the surrounding copy.
    case FooterPage::Live:
        return std::format(L"{:.0f} dps | {} players", data.live_dps, data.live_players);
    case FooterPage::HistoryList:
        return std::format(L"{} encounters", data.history_count);
    case FooterPage::HistoryDetail:
        return std::format(L"{:.0f} dps | {} players",
            data.history_dps, data.history_players);
    }
    return {};
}

WindowExtent ScaleWindowExtent(
    const int logical_width,
    const int logical_height,
    const float dpi_scale,
    const float ui_scale) noexcept {
    const float scale = std::clamp(dpi_scale, 0.5F, 4.0F) *
        std::clamp(ui_scale, 0.75F, 2.0F);
    return {
        std::max(1, static_cast<int>(std::lround(
            static_cast<float>(logical_width) * scale))),
        std::max(1, static_cast<int>(std::lround(
            static_cast<float>(logical_height) * scale))),
    };
}

WindowExtent UnscaleWindowExtent(
    const int physical_width,
    const int physical_height,
    const float dpi_scale,
    const float ui_scale) noexcept {
    const float scale = std::clamp(dpi_scale, 0.5F, 4.0F) *
        std::clamp(ui_scale, 0.75F, 2.0F);
    return {
        std::max(1, static_cast<int>(std::lround(
            static_cast<float>(physical_width) / scale))),
        std::max(1, static_cast<int>(std::lround(
            static_cast<float>(physical_height) / scale))),
    };
}

HitRegion HitTest(
    const LayoutMetrics& layout,
    const float x,
    const float y,
    const bool locked) noexcept {
    if (locked) return HitRegion::Client;
    const bool left = x >= 0.0F && x < layout.resize_grip;
    const bool right = x >= layout.width - layout.resize_grip && x <= layout.width;
    const bool top = y >= 0.0F && y < layout.resize_grip;
    const bool bottom = y >= layout.height - layout.resize_grip && y <= layout.height;
    if (left && top) return HitRegion::ResizeTopLeft;
    if (right && top) return HitRegion::ResizeTopRight;
    if (left && bottom) return HitRegion::ResizeBottomLeft;
    if (right && bottom) return HitRegion::ResizeGrip;
    if (left) return HitRegion::ResizeLeft;
    if (right) return HitRegion::ResizeRight;
    if (top) return HitRegion::ResizeTop;
    if (bottom) return HitRegion::ResizeBottom;
    if (y >= 0.0F && y < layout.title_height) return HitRegion::Title;
    if (y >= layout.title_height &&
        y < layout.title_height + layout.header_height) {
        return HitRegion::ColumnHeader;
    }
    return HitRegion::Client;
}

HitRegion ResolveWindowHit(
    const HitRegion base_region,
    const float x,
    const float y,
    const std::span<const ViewRect> interactive_rects) noexcept {
    switch (base_region) {
    case HitRegion::ResizeLeft:
    case HitRegion::ResizeRight:
    case HitRegion::ResizeTop:
    case HitRegion::ResizeBottom:
    case HitRegion::ResizeTopLeft:
    case HitRegion::ResizeTopRight:
    case HitRegion::ResizeBottomLeft:
    case HitRegion::ResizeGrip:
        return base_region;
    default:
        break;
    }
    if (base_region == HitRegion::Title) {
        for (const auto& rect : interactive_rects) {
            if (x >= rect.left && x <= rect.right &&
                y >= rect.top && y <= rect.bottom) {
                return HitRegion::Client;
            }
        }
    }
    return base_region;
}

void ReconcileLiveNavigation(
    NavigationState& state,
    const std::string_view encounter_key,
    const std::span<const std::string> player_keys,
    const std::span<const std::string> source_keys) noexcept {
    if (state.page != ViewPage::Live) return;
    if (state.encounter_key != encounter_key) {
        state.encounter_key.assign(encounter_key);
        state.drill = DrillLevel::Players;
        state.player_key.clear();
        state.source_key.clear();
        state.scroll_offset = 0;
        return;
    }
    if (state.drill != DrillLevel::Players && !player_keys.empty() &&
        std::ranges::find(player_keys, state.player_key) == player_keys.end()) {
        state.drill = DrillLevel::Players;
        state.player_key.clear();
        state.source_key.clear();
        state.scroll_offset = 0;
        return;
    }
    if (state.drill == DrillLevel::Events && !source_keys.empty() &&
        std::ranges::find(source_keys, state.source_key) == source_keys.end()) {
        state.drill = DrillLevel::Sources;
        state.source_key.clear();
        state.scroll_offset = 0;
    }
}

}  // namespace nte::combat_stats::overlay
