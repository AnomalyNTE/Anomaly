#pragma once

#include "attribute_color_map.hpp"
#include "overlay_protocol.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nte::combat_stats::overlay {

enum class SortColumn : std::uint8_t {
    Character,
    Damage,
    Share,
    EncounterDps,
    CriticalRate,
    MaximumHit,
    HeadRate,
    WeakRate,
    ActiveDps,
    RollingDps,
};

enum class HealingDirection : std::uint8_t { Done, Received };

struct ColumnVisibility {
    bool head_rate{};
    bool weak_rate{};
    bool active_dps{};
    bool rolling_dps{};
};

struct LayoutMetrics {
    float width{};
    float height{};
    float scale{1.0F};
    float title_height{};
    float header_height{};
    float row_height{};
    float footer_height{};
    float resize_grip{};
    std::size_t visible_rows{};
};

struct ReferenceTheme {
    std::uint32_t title_background{};
    std::uint32_t header_background{};
    std::uint32_t content_background{};
    std::uint32_t footer_background{};
    std::uint32_t text{};
    std::uint32_t muted_text{};
    std::uint32_t accent{};
    std::uint32_t separator{};
};

struct ReferenceMetrics {
    float title_height{};
    float header_height{};
    float row_height{};
    float footer_height{};
    float resize_grip{};
    float player_name_inset{};
    float cell_horizontal_padding{};
    float player_glyph_left{};
    float player_glyph_right{};
    float history_icon_font_size{};
    float footer_tab_width{};
    float footer_content_gap{};
    float footer_safe_padding{};
};

struct ViewRect {
    float left{};
    float top{};
    float right{};
    float bottom{};
};

struct TitleBarLayout {
    ViewRect history;
    ViewRect manual_end;
    ViewRect time;
    ViewRect title;
    ViewRect target;
    ViewRect collapse;
    ViewRect menu;
};

enum class TextAlignment : std::uint8_t { Center, Trailing };

struct TextStyle {
    std::wstring_view font_family;
    float logical_size{};
    TextAlignment alignment{TextAlignment::Center};
};

struct OverlayTextStyles {
    TextStyle regular_text;
    TextStyle small_text;
    TextStyle bold_text;
    TextStyle tab_text;
    TextStyle footer_text;
    TextStyle history_icon_text;
};

struct FooterLayout {
    ViewRect outgoing_tab;
    ViewRect incoming_tab;
    ViewRect healing_done_tab;
    ViewRect healing_received_tab;
    ViewRect summary;
    ViewRect resize_grip;
};

struct SummaryColumn {
    // Catalog key; the overlay resolves it, keeping this library free of the
    // process-wide text catalog.
    std::string_view key;
    std::wstring_view fallback;
    SortColumn sort;
    float weight;
};

enum class FooterPage : std::uint8_t { Live, HistoryList, HistoryDetail };

struct FooterData {
    std::size_t live_players{};
    double live_dps{};
    std::size_t history_count{};
    std::size_t history_players{};
    double history_dps{};
};

struct WindowExtent {
    int width{};
    int height{};
};

enum class HitRegion : std::uint8_t {
    Client,
    Title,
    ColumnHeader,
    ResizeLeft,
    ResizeRight,
    ResizeTop,
    ResizeBottom,
    ResizeTopLeft,
    ResizeTopRight,
    ResizeBottomLeft,
    ResizeGrip,
};

enum class ViewPage : std::uint8_t { Live, HistoryList, HistoryDetail };
enum class DrillLevel : std::uint8_t { Players, Sources, Events };

struct NavigationState {
    ViewPage page{ViewPage::Live};
    DrillLevel drill{DrillLevel::Players};
    std::string encounter_key;
    std::string player_key;
    std::string source_key;
    std::size_t scroll_offset{};
};

[[nodiscard]] std::vector<PlayerRow> SortPlayers(
    const std::vector<PlayerRow>& players,
    SortColumn column,
    bool descending);
[[nodiscard]] std::vector<PlayerRow> SortHealingPlayers(
    const std::vector<PlayerRow>& players,
    HealingDirection direction);
[[nodiscard]] std::int64_t HealingAmount(
    const PlayerRow& player, HealingDirection direction) noexcept;
[[nodiscard]] std::uint64_t HealingEventCount(
    const PlayerRow& player, HealingDirection direction) noexcept;
[[nodiscard]] double HealingAverage(
    const PlayerRow& player, HealingDirection direction) noexcept;
[[nodiscard]] LayoutMetrics CalculateLayout(
    float width,
    float height,
    float scale,
    bool collapsed,
    std::size_t player_count) noexcept;
[[nodiscard]] WindowExtent ScaleWindowExtent(
    int logical_width,
    int logical_height,
    float dpi_scale,
    float ui_scale) noexcept;
[[nodiscard]] WindowExtent UnscaleWindowExtent(
    int physical_width,
    int physical_height,
    float dpi_scale,
    float ui_scale) noexcept;
[[nodiscard]] HitRegion HitTest(
    const LayoutMetrics& layout,
    float x,
    float y,
    bool locked) noexcept;
[[nodiscard]] HitRegion ResolveWindowHit(
    HitRegion base_region,
    float x,
    float y,
    std::span<const ViewRect> interactive_rects) noexcept;
void ReconcileLiveNavigation(
    NavigationState& state,
    std::string_view encounter_key,
    std::span<const std::string> player_keys = {},
    std::span<const std::string> source_keys = {}) noexcept;
[[nodiscard]] ReferenceTheme OverlayReferenceTheme() noexcept;
[[nodiscard]] ReferenceMetrics OverlayReferenceMetrics() noexcept;
[[nodiscard]] OverlayTextStyles OverlayReferenceTextStyles() noexcept;
[[nodiscard]] bool ShouldHideOverlay(
    bool hide_idle,
    std::optional<EncounterState> state) noexcept;
[[nodiscard]] bool CanExportEncounter(
    ViewPage page,
    std::optional<EncounterState> live_state,
    bool history_detail_ready) noexcept;
[[nodiscard]] TitleBarLayout CalculateTitleBarLayout(
    float width, float scale, float title_height) noexcept;
[[nodiscard]] ViewRect CalculateCellTextRect(
    ViewRect cell, float scale, float leading_content_inset) noexcept;
[[nodiscard]] ViewRect CalculatePlayerGlyphRect(
    ViewRect cell, float scale) noexcept;
[[nodiscard]] FooterLayout CalculateFooterLayout(
    float width,
    float height,
    float scale,
    float footer_height,
    float resize_grip,
    bool show_tabs) noexcept;
[[nodiscard]] std::uint32_t StablePlayerColor(std::string_view identity) noexcept;
[[nodiscard]] std::uint32_t PlayerDisplayColor(
    std::optional<std::uint32_t> attribute_color,
    std::string_view identity) noexcept;
[[nodiscard]] std::uint32_t ResolvePlayerDisplayColor(
    const PlayerRow& player,
    const AttributeColorMap& colors);
[[nodiscard]] double PlayerBarFraction(
    std::int64_t damage, std::int64_t maximum_damage) noexcept;
[[nodiscard]] const std::array<SummaryColumn, 6>& OutgoingSummaryColumns() noexcept;
[[nodiscard]] bool IsOutgoingSummarySort(SortColumn column) noexcept;
[[nodiscard]] long long RoundedPercent(double value) noexcept;
[[nodiscard]] float PlayerNameInset(float scale) noexcept;
[[nodiscard]] std::wstring FooterSummary(FooterPage page, const FooterData& data);

}  // namespace nte::combat_stats::overlay
