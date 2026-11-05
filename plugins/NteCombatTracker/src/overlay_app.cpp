#include "attribute_color_map.hpp"
#include "app_settings.hpp"
#include "combat_export.hpp"
#include "overlay_control.hpp"
#include "overlay_model.hpp"
#include "overlay_protocol.hpp"
#include "overlay_text_catalog.hpp"
#include "history_reader.hpp"
#include "log_worker.hpp"
#include "plugin_version.hpp"

#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <winhttp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;
using nte::combat_stats::EncounterState;
using nte::combat_stats::Handle;
using nte::combat_stats::NteOverlaySnapshot;
using nte::combat_stats::PlayerRow;
using nte::combat_stats::overlay::CalculateLayout;
using nte::combat_stats::PlayerCriticalRate;
using nte::combat_stats::PlayerHeadRate;
using nte::combat_stats::PlayerWeakRate;
using nte::combat_stats::overlay::HitRegion;
using nte::combat_stats::overlay::HitTest;
using nte::combat_stats::overlay::SortColumn;
using nte::combat_stats::overlay::SortPlayers;
using nte::combat_stats::overlay::ScaleWindowExtent;
using nte::combat_stats::overlay::UnscaleWindowExtent;
using nte::combat_stats::overlay::kWindowClassName;
using nte::combat_stats::overlay::kWindowTitle;
using namespace std::chrono_literals;

constexpr wchar_t kMutexName[] = L"Local\\NteCombatOverlay.shikari.nte-combat-tracker";
constexpr UINT kSnapshotMessage = WM_APP + 1;
constexpr UINT kTrayMessage = WM_APP + 2;
constexpr UINT kShowMessage = WM_APP + 3;
constexpr UINT kRestartMessage = WM_APP + 4;
constexpr UINT kHistoryRefreshMessage = WM_APP + 5;
constexpr UINT kExportCompleteMessage = WM_APP + 6;
constexpr UINT kHotkeyId = 1;
constexpr UINT_PTR kStaleTimerId = 1;
constexpr UINT kTrayId = 1;
constexpr std::size_t kMaximumMessage = nte::combat_stats::kMaximumOverlayMessageBytes;
UINT g_reload_settings_message{};
UINT g_close_overlay_message{};

enum MenuId : UINT {
    MenuToggleLock = 100,
    MenuToggleCollapsed,
    MenuToggleTopmost,
    MenuToggleHideIdle,
    MenuOpacity60,
    MenuOpacity75,
    MenuOpacity90,
    MenuScale100,
    MenuScale125,
    MenuScale150,
    MenuExportCurrent,
    MenuShow,
    MenuHide,
    MenuExit,
};

using DataTab = nte::settings::DataTab;
enum class ViewPage : std::uint8_t { Live, HistoryList, HistoryDetail };
enum class DrillLevel : std::uint8_t { Players, Sources, Events };
enum class UiAction : std::uint8_t {
    ManualEnd,
    ToggleHistory,
    ToggleCollapsed,
    OpenMenu,
    Back,
    OutgoingTab,
    IncomingTab,
    HealingDoneTab,
    HealingReceivedTab,
    SelectRow,
};

struct HitTarget {
    D2D1_RECT_F rect{};
    UiAction action{};
    std::size_t index{};
};

struct RenderResources {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> write;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IDWriteTextFormat> regular;
    ComPtr<IDWriteTextFormat> small_format;
    ComPtr<IDWriteTextFormat> bold;
};

struct App {
    HINSTANCE instance{};
    HWND window{};
    UINT taskbar_created{};
    std::uint16_t port{14514};
    nte::settings::OverlaySettings settings;
    std::filesystem::path settings_path;
    RenderResources render;
    std::jthread receiver;
    std::jthread lifetime_watcher;
    std::jthread history_loader;
    std::jthread export_worker;
    std::mutex socket_mutex;
    HINTERNET active_socket{};
    HINTERNET active_request{};
    HINTERNET cancelled_request{};
    std::mutex snapshot_mutex;
    std::optional<NteOverlaySnapshot> snapshot;
    // Replaced by the catalog value once the first snapshot arrives; the
    // literal here is the English fallback used before that.
    std::string connection_status{"Connecting to the Runtime…"};
    std::chrono::steady_clock::time_point last_snapshot{};
    std::atomic_bool lifetime_guarded{};
    bool connected{};
    bool dragging{};
    POINT drag_origin{};
    RECT window_origin{};
    HANDLE manual_end_event{};
    ViewPage page{ViewPage::Live};
    DrillLevel drill{DrillLevel::Players};
    nte::combat_stats::overlay::NavigationState navigation;
    Handle selected_player;
    nte::combat_stats::overlay::EventSourceSummary selected_source;
    std::size_t scroll_offset{};
    std::size_t maximum_scroll{};
    std::optional<std::size_t> hovered_target;
    std::vector<HitTarget> hit_targets;
    std::vector<nte::combat_stats::overlay::AccountHistoryGroup> history;
    std::optional<nte::combat_stats::overlay::EncounterDetail> history_detail;
    std::vector<nte::combat_stats::overlay::CombatEventDetail> live_events;
    std::string requested_report_id;
    std::string requested_encounter_id;
    nte::account::AccountIdentity requested_account;
    std::string loaded_live_report_id;
    std::string loaded_live_encounter_id;
    nte::account::AccountIdentity loaded_live_account;
    std::string history_error;
    std::string action_status;
    std::string export_result_status;
    bool export_in_progress{};
    std::vector<PlayerRow> displayed_players;
    std::vector<nte::combat_stats::overlay::EventSourceSummary> displayed_sources;
    std::vector<nte::combat_stats::overlay::CombatEventDetail> displayed_events;
    std::vector<nte::combat_stats::overlay::HistoryListRow> displayed_history;
    std::unordered_set<std::string> expanded_history_accounts;
    bool history_expansion_initialized{};
    nte::combat_stats::AttributeColorMap attribute_colors;
    std::string attribute_color_error;
    // Overlay copy resolved from the catalog the plugin ships. Lookups fall back
    // to the English literal at each call site, so a missing catalog degrades to
    // English rather than blank text.
    nte::combat_stats::overlay::TextCatalog text;
    std::string text_locale;
    std::filesystem::path executable_root;
};

std::wstring Utf16(const std::string& text);

App g_app;
// Catalog patterns carry `{0}` placeholders. std::format needs a compile-time
// pattern while catalog text is only known at run time, so this uses vformat
// rather than string concatenation: the placeholder stays in the catalog and the
// value is substituted, which is what the i18n rules require.
[[nodiscard]] std::wstring Substitute(const std::wstring_view pattern,
                                      const std::wstring_view value) {
    return std::vformat(pattern, std::make_wformat_args(value));
}

// Narrow variant for messages that are stored as UTF-8.
[[nodiscard]] std::string Substitute(const std::string_view pattern,
                                     const std::string_view value) {
    return std::vformat(pattern, std::make_format_args(value));
}
// This process has no Host services, so it cannot construct an
// anomaly::sdk::Localizer. The adapter resolves the same catalog file the
// plugin ships; the plugin stays the single Host-service Localizer source.
// Call sites go through this adapter with a literal key and English fallback,
// so the i18n gate can match both literally.
struct OverlayCatalogText {
    [[nodiscard]] std::wstring Text(const std::string_view catalog_key,
                                    const std::string_view fallback) const {
        return g_app.text.Text(catalog_key, fallback);
    }
};
const OverlayCatalogText localizer;

// UTF-16 to UTF-8 for values that are stored narrow (status strings, chrome).
[[nodiscard]] std::string Narrow(const std::wstring_view wide) {
    if (wide.empty()) return {};
    const int count = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string narrow(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        narrow.data(), count, nullptr, nullptr);
    return narrow;
}

std::size_t HistoryEncounterCount(
    const std::vector<nte::combat_stats::overlay::AccountHistoryGroup>& groups) {
    std::size_t count{};
    for (const auto& group : groups) count += group.encounters.size();
    return count;
}

std::wstring AccountLabel(const nte::account::AccountIdentity& account) {
    return account.known() ? L"UID " + Utf16(account.uid)
                           : localizer.Text("overlay.account.unknown", "Unknown account");
}

std::wstring Utf16(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return L"?";
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::string PlayerKey(const PlayerRow& player) {
    return player.identity_key.empty() ? player.label : player.identity_key;
}

std::string SourceKey(
    const nte::combat_stats::overlay::EventSourceSummary& source) {
    return std::to_string(source.name_id) + "|" + source.source_name + "|" +
        std::to_string(source.participant.id) + ":" +
        std::to_string(source.participant.generation);
}

bool LoadSettings() {
    g_app.settings_path = nte::settings::OverlaySettingsPath();
    std::string error;
    const bool loaded = nte::settings::LoadOverlaySettings(
        g_app.settings_path, g_app.settings, error);
    if (!nte::combat_stats::overlay::IsOutgoingSummarySort(
            g_app.settings.sort_column)) {
        g_app.settings.sort_column = SortColumn::Damage;
        g_app.settings.descending = true;
    }
    return loaded;
}

void SaveSettings() {
    if (g_app.settings_path.empty()) return;
    RECT rect{};
    if (g_app.window != nullptr && GetWindowRect(g_app.window, &rect)) {
        g_app.settings.x = rect.left;
        g_app.settings.y = rect.top;
        if (!g_app.settings.collapsed) {
            const float dpi_scale =
                static_cast<float>(GetDpiForWindow(g_app.window)) / 96.0F;
            const auto logical = UnscaleWindowExtent(
                rect.right - rect.left, rect.bottom - rect.top,
                dpi_scale, g_app.settings.ui_scale);
            g_app.settings.width = logical.width;
            g_app.settings.height = logical.height;
        }
    }
    std::string error;
    static_cast<void>(nte::settings::SaveOverlaySettings(
        g_app.settings_path, g_app.settings, error));
}

void AddTrayIcon() {
    NOTIFYICONDATAW icon{sizeof(icon)};
    icon.hWnd = g_app.window;
    icon.uID = kTrayId;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon.uCallbackMessage = kTrayMessage;
    icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(icon.szTip, kWindowTitle);
    Shell_NotifyIconW(NIM_ADD, &icon);
}

void RemoveTrayIcon() {
    NOTIFYICONDATAW icon{sizeof(icon)};
    icon.hWnd = g_app.window;
    icon.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &icon);
}

void ApplyWindowMode() {
    LONG_PTR extended = GetWindowLongPtrW(g_app.window, GWL_EXSTYLE);
    extended |= WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (g_app.settings.locked) extended |= WS_EX_TRANSPARENT;
    else extended &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtrW(g_app.window, GWL_EXSTYLE, extended);
    SetWindowPos(g_app.window, g_app.settings.topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

void DrawTextLine(ID2D1RenderTarget* target, IDWriteTextFormat* format,
                  ID2D1Brush* brush, const std::wstring& text,
                  const D2D1_RECT_F rect) {
    target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format,
        rect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

DWRITE_TEXT_ALIGNMENT DWriteTextAlignment(
    const nte::combat_stats::overlay::TextAlignment alignment) noexcept {
    switch (alignment) {
    case nte::combat_stats::overlay::TextAlignment::Center:
        return DWRITE_TEXT_ALIGNMENT_CENTER;
    case nte::combat_stats::overlay::TextAlignment::Trailing:
        return DWRITE_TEXT_ALIGNMENT_TRAILING;
    }
    return DWRITE_TEXT_ALIGNMENT_CENTER;
}

bool ConfigureTextFormat(
    IDWriteTextFormat* format,
    const nte::combat_stats::overlay::TextAlignment alignment) {
    return SUCCEEDED(format->SetTextAlignment(DWriteTextAlignment(alignment))) &&
        SUCCEEDED(format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER)) &&
        SUCCEEDED(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));
}

std::wstring CompactNumber(const double value) {
    const double absolute = std::abs(value);
    if (absolute >= 1'000'000'000.0) return std::format(L"{:.2f}B", value / 1'000'000'000.0);
    if (absolute >= 1'000'000.0) return std::format(L"{:.2f}M", value / 1'000'000.0);
    if (absolute >= 1'000.0) return std::format(L"{:.1f}K", value / 1'000.0);
    return std::format(L"{:.0f}", value);
}

std::wstring StateText(const NteOverlaySnapshot& snapshot) {
    if (!snapshot.capture_available) {
        return localizer.Text("overlay.state.capture_unavailable", "Capture unavailable");
    }
    switch (snapshot.combat.state) {
    case EncounterState::Idle: return localizer.Text("overlay.state.idle", "Waiting for combat");
    case EncounterState::Active: return localizer.Text("overlay.state.active", "Active");
    case EncounterState::Closing: return localizer.Text("overlay.state.closing", "Closing log");
    case EncounterState::Completed: return localizer.Text("overlay.state.completed", "Completed");
    }
    return localizer.Text("overlay.state.idle", "Waiting for combat");
}

std::wstring CellText(const PlayerRow& row, const SortColumn column) {
    switch (column) {
    case SortColumn::Character: return Utf16(row.label);
    case SortColumn::Damage: return CompactNumber(static_cast<double>(row.damage));
    case SortColumn::Share: return std::format(L"{:.1f}%", row.damage_share);
    case SortColumn::EncounterDps: return CompactNumber(row.encounter_dps);
    case SortColumn::CriticalRate: return std::format(L"{:.1f}%", PlayerCriticalRate(row));
    case SortColumn::MaximumHit: return CompactNumber(static_cast<double>(row.maximum_hit));
    case SortColumn::HeadRate: return std::format(L"{:.1f}%", PlayerHeadRate(row));
    case SortColumn::WeakRate: return std::format(L"{:.1f}%", PlayerWeakRate(row));
    case SortColumn::ActiveDps: return CompactNumber(row.active_dps);
    case SortColumn::RollingDps: return CompactNumber(row.rolling_dps);
    }
    return {};
}

std::wstring TimeText(const std::int64_t utc_ms) {
    const std::time_t seconds = static_cast<std::time_t>(utc_ms / 1000);
    std::tm local{};
    if (localtime_s(&local, &seconds) != 0) return L"--:--:--";
    return std::format(L"{:02}-{:02} {:02}:{:02}:{:02}",
        local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
}

std::wstring EventTimeText(const std::int64_t utc_ms, const double relative_seconds) {
    const std::time_t seconds = static_cast<std::time_t>(utc_ms / 1000);
    std::tm local{};
    if (localtime_s(&local, &seconds) != 0) {
        return std::format(L"+{:.3f}s", relative_seconds);
    }
    const auto milliseconds = static_cast<int>((utc_ms % 1000 + 1000) % 1000);
    return std::format(L"+{:.3f}s {:02}:{:02}:{:02}.{:03}", relative_seconds,
        local.tm_hour, local.tm_min, local.tm_sec, milliseconds);
}

bool Contains(const D2D1_RECT_F& rect, const float x, const float y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

std::vector<nte::combat_stats::overlay::CombatEventDetail> CanonicalPlayerEvents(
    std::vector<nte::combat_stats::overlay::CombatEventDetail> events,
    const PlayerRow& player) {
    nte::combat_stats::overlay::CanonicalizePlayerEvents(events, {player});
    return events;
}

std::vector<nte::combat_stats::overlay::CombatEventDetail> MergeLiveEvents(
    const NteOverlaySnapshot& snapshot,
    std::vector<nte::combat_stats::overlay::CombatEventDetail> events);

void RenderOverlay() {
    RECT client{};
    GetClientRect(g_app.window, &client);
    const UINT width = static_cast<UINT>(std::max(1L, client.right));
    const UINT height = static_cast<UINT>(std::max(1L, client.bottom));
    if (!g_app.render.wic || !g_app.render.d2d) return;

    ComPtr<IWICBitmap> bitmap;
    if (FAILED(g_app.render.wic->CreateBitmap(width, height,
            GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) return;
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1RenderTarget> target;
    if (FAILED(g_app.render.d2d->CreateWicBitmapRenderTarget(
            bitmap.Get(), properties, &target))) return;
    ComPtr<ID2D1SolidColorBrush> text;
    ComPtr<ID2D1SolidColorBrush> muted;
    ComPtr<ID2D1SolidColorBrush> accent;
    ComPtr<ID2D1SolidColorBrush> hover;
    ComPtr<ID2D1SolidColorBrush> warning;
    ComPtr<ID2D1SolidColorBrush> title_background;
    ComPtr<ID2D1SolidColorBrush> header_background;
    ComPtr<ID2D1SolidColorBrush> footer_background;
    ComPtr<ID2D1SolidColorBrush> separator;
    const auto theme = nte::combat_stats::overlay::OverlayReferenceTheme();
    target->CreateSolidColorBrush(D2D1::ColorF(theme.text, 1.0F), &text);
    target->CreateSolidColorBrush(D2D1::ColorF(theme.muted_text, 1.0F), &muted);
    target->CreateSolidColorBrush(D2D1::ColorF(theme.accent, 1.0F), &accent);
    target->CreateSolidColorBrush(D2D1::ColorF(theme.accent, 0.20F), &hover);
    target->CreateSolidColorBrush(D2D1::ColorF(0xFFB454, 1.0F), &warning);
    target->CreateSolidColorBrush(D2D1::ColorF(
        theme.title_background, g_app.settings.opacity), &title_background);
    target->CreateSolidColorBrush(D2D1::ColorF(
        theme.header_background, g_app.settings.opacity), &header_background);
    target->CreateSolidColorBrush(D2D1::ColorF(
        theme.footer_background, g_app.settings.opacity), &footer_background);
    target->CreateSolidColorBrush(D2D1::ColorF(theme.separator, 1.0F), &separator);

    std::optional<NteOverlaySnapshot> snapshot;
    std::optional<nte::combat_stats::overlay::EncounterDetail> history_detail;
    std::vector<nte::combat_stats::overlay::AccountHistoryGroup> history;
    std::vector<nte::combat_stats::overlay::CombatEventDetail> live_events;
    std::string status;
    std::string history_error;
    bool connected{};
    {
        std::scoped_lock lock(g_app.snapshot_mutex);
        if (g_app.snapshot) snapshot.emplace(*g_app.snapshot);
        if (g_app.history_detail) history_detail.emplace(*g_app.history_detail);
        history = g_app.history;
        live_events = g_app.live_events;
        status = g_app.connection_status;
        history_error = g_app.history_error;
        connected = g_app.connected;
    }
    const float dpi = static_cast<float>(GetDpiForWindow(g_app.window)) / 96.0F;
    const float scale = dpi * g_app.settings.ui_scale;
    const std::size_t player_count = snapshot ? snapshot->combat.players.size() : 0;
    const auto layout = CalculateLayout(
        static_cast<float>(width), static_cast<float>(height), scale,
        g_app.settings.collapsed, player_count);
    ComPtr<IDWriteTextFormat> scaled_regular;
    ComPtr<IDWriteTextFormat> scaled_small;
    ComPtr<IDWriteTextFormat> scaled_bold;
    ComPtr<IDWriteTextFormat> scaled_tab;
    ComPtr<IDWriteTextFormat> scaled_footer;
    ComPtr<IDWriteTextFormat> scaled_history_icon;
    const auto text_styles = nte::combat_stats::overlay::OverlayReferenceTextStyles();
    auto scaled_format = [&](const nte::combat_stats::overlay::TextStyle style,
                             const DWRITE_FONT_WEIGHT weight,
                             ComPtr<IDWriteTextFormat>& output) {
        if (FAILED(g_app.render.write->CreateTextFormat(style.font_family.data(), nullptr,
                weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                style.logical_size * scale, L"zh-CN", &output))) return false;
        return ConfigureTextFormat(output.Get(), style.alignment);
    };
    if (!scaled_format(text_styles.regular_text,
            DWRITE_FONT_WEIGHT_NORMAL, scaled_regular) ||
        !scaled_format(text_styles.small_text,
            DWRITE_FONT_WEIGHT_NORMAL, scaled_small) ||
        !scaled_format(text_styles.bold_text,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, scaled_bold) ||
        !scaled_format(text_styles.tab_text,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, scaled_tab) ||
        !scaled_format(text_styles.footer_text,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, scaled_footer) ||
        !scaled_format(text_styles.history_icon_text,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, scaled_history_icon)) return;

    target->BeginDraw();
    target->Clear(D2D1::ColorF(theme.content_background, g_app.settings.opacity));
    target->FillRectangle(D2D1::RectF(0, 0, layout.width, layout.title_height),
        title_background.Get());
    g_app.hit_targets.clear();
    g_app.displayed_players.clear();
    g_app.displayed_sources.clear();
    g_app.displayed_events.clear();
    g_app.displayed_history.clear();
    const auto add_target = [&](const D2D1_RECT_F rect, const UiAction action,
                                const std::size_t index = 0) {
        g_app.hit_targets.push_back({rect, action, index});
    };
    const auto to_d2d_rect = [](const nte::combat_stats::overlay::ViewRect rect) {
        return D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom);
    };
    const auto draw_cells = [&](const std::vector<std::wstring>& cells,
                                const std::vector<float>& weights,
                                const D2D1_RECT_F row, IDWriteTextFormat* format,
                                ID2D1Brush* brush,
                                const bool header = false,
                                const float first_cell_inset = 0.0F) {
        if (header) {
            target->FillRectangle(row, header_background.Get());
            target->DrawLine(D2D1::Point2F(row.left, row.bottom - scale),
                D2D1::Point2F(row.right, row.bottom - scale), separator.Get(), scale);
        }
        float total{};
        for (const float weight : weights) total += weight;
        float x = row.left;
        for (std::size_t index{}; index < cells.size(); ++index) {
            const float next = x + (row.right - row.left) * weights[index] / total;
            const auto text_rect = nte::combat_stats::overlay::CalculateCellTextRect(
                {x, row.top, next, row.bottom}, scale,
                index == 0 ? first_cell_inset : 0.0F);
            DrawTextLine(target.Get(), format, brush, cells[index],
                to_d2d_rect(text_rect));
            x = next;
        }
    };
    const auto title_layout = nte::combat_stats::overlay::CalculateTitleBarLayout(
        layout.width, scale, layout.title_height);
    const D2D1_RECT_F history_rect = to_d2d_rect(title_layout.history);
    DrawTextLine(target.Get(), scaled_history_icon.Get(), accent.Get(), L"◷", history_rect);
    add_target(history_rect, UiAction::ToggleHistory);
    const D2D1_RECT_F manual_rect = to_d2d_rect(title_layout.manual_end);
    const bool manual_enabled = g_app.page == ViewPage::Live && snapshot &&
        snapshot->combat.state == EncounterState::Active;
    if (manual_enabled) {
        target->FillRectangle(manual_rect, hover.Get());
        add_target(manual_rect, UiAction::ManualEnd);
    }
    DrawTextLine(target.Get(), scaled_small.Get(), manual_enabled ? text.Get() : muted.Get(),
        localizer.Text("overlay.button.end", "End"), manual_rect);
    std::wstring title = g_app.page == ViewPage::Live
        ? localizer.Text("overlay.title.live", "Current encounter")
        : localizer.Text("overlay.title.history", "Encounter history");
    std::wstring time = L"00:00";
    std::wstring target_name;
    if (g_app.page == ViewPage::HistoryDetail && history_detail) {
        const int seconds = static_cast<int>(history_detail->summary.duration_seconds);
        time = std::format(L"{:02}:{:02}", seconds / 60, seconds % 60);
        target_name = Utf16(history_detail->summary.end_reason);
        if (!history_detail->summary.complete) {
            target_name += L" ⚠ " + localizer.Text("overlay.badge.incomplete", "data incomplete");
        }
    } else if (g_app.page == ViewPage::HistoryList) {
        target_name = Substitute(
            localizer.Text("overlay.history.count", "{0} completed encounters"),
            std::to_wstring(HistoryEncounterCount(history)));
        if (!history_error.empty()) {
            target_name += L" ⚠ " + Utf16(history_error);
        }
    } else if (snapshot) {
        const int seconds = static_cast<int>(snapshot->combat.duration_seconds);
        time = std::format(L"{:02}:{:02}", seconds / 60, seconds % 60);
        target_name = snapshot->primary_target.empty()
            ? (connected ? StateText(*snapshot)
                         : localizer.Text("overlay.state.disconnected", "Disconnected"))
            : Utf16(snapshot->primary_target);
        if (snapshot->combat.quality.partial || snapshot->combat.quality.overflow ||
            snapshot->combat.quality.cursor_gap ||
            snapshot->combat.quality.aggregate_truncated) target_name += L" ⚠";
        const bool healing_tab =
            g_app.settings.active_tab == DataTab::HealingDone ||
            g_app.settings.active_tab == DataTab::HealingReceived;
        if (healing_tab && snapshot->healing_capture_status != "available") {
            target_name += L" | " +
                localizer.Text("overlay.badge.healing_unavailable", "healing capture unavailable");
        }
    }
    if (!g_app.action_status.empty()) target_name = Utf16(g_app.action_status);
    if (!g_app.attribute_color_error.empty()) {
        target_name += L" | " +
            localizer.Text("overlay.badge.attribute_unavailable", "attribute colors unavailable");
    }
    DrawTextLine(target.Get(), scaled_bold.Get(), accent.Get(), time,
        to_d2d_rect(title_layout.time));
    DrawTextLine(target.Get(), scaled_bold.Get(), text.Get(), title,
        to_d2d_rect(title_layout.title));
    DrawTextLine(target.Get(), scaled_regular.Get(), muted.Get(), target_name,
        to_d2d_rect(title_layout.target));
    const D2D1_RECT_F collapse_rect = to_d2d_rect(title_layout.collapse);
    const float collapse_center_x = (collapse_rect.left + collapse_rect.right) / 2.0F;
    const float collapse_center_y = (collapse_rect.top + collapse_rect.bottom) / 2.0F;
    const float collapse_outer_y = collapse_center_y +
        (g_app.settings.collapsed ? -2.0F : 2.0F) * scale;
    const float collapse_tip_y = collapse_center_y +
        (g_app.settings.collapsed ? 2.0F : -2.0F) * scale;
    const D2D1_POINT_2F collapse_tip =
        D2D1::Point2F(collapse_center_x, collapse_tip_y);
    const float collapse_stroke = (std::max)(1.0F, 1.5F * scale);
    target->DrawLine(
        D2D1::Point2F(collapse_center_x - 4.0F * scale, collapse_outer_y),
        collapse_tip, text.Get(), collapse_stroke);
    target->DrawLine(collapse_tip,
        D2D1::Point2F(collapse_center_x + 4.0F * scale, collapse_outer_y),
        text.Get(), collapse_stroke);
    add_target(collapse_rect, UiAction::ToggleCollapsed);
    const D2D1_RECT_F menu_rect = to_d2d_rect(title_layout.menu);
    DrawTextLine(target.Get(), scaled_bold.Get(), text.Get(), L"⋮", menu_rect);
    add_target(menu_rect, UiAction::OpenMenu);

    if (!g_app.settings.collapsed) {
        const float tab_height = layout.footer_height;
        const float row_height = layout.row_height;
        const float header_height = layout.header_height;
        float top = layout.title_height;
        const bool show_tabs = g_app.page != ViewPage::HistoryList;
        const float content_bottom = layout.height - tab_height;
        const auto footer_layout = nte::combat_stats::overlay::CalculateFooterLayout(
            layout.width, layout.height, scale, tab_height,
            layout.resize_grip, show_tabs);
        target->FillRectangle(D2D1::RectF(0, content_bottom,
            layout.width, layout.height), footer_background.Get());
        if (show_tabs) {
            const D2D1_RECT_F outgoing = to_d2d_rect(footer_layout.outgoing_tab);
            const D2D1_RECT_F incoming = to_d2d_rect(footer_layout.incoming_tab);
            const D2D1_RECT_F healing_done = to_d2d_rect(footer_layout.healing_done_tab);
            const D2D1_RECT_F healing_received = to_d2d_rect(footer_layout.healing_received_tab);
            DrawTextLine(target.Get(), scaled_tab.Get(),
                g_app.settings.active_tab == DataTab::Outgoing ? accent.Get() : text.Get(),
                localizer.Text("overlay.tab.outgoing", "Damage"), outgoing);
            DrawTextLine(target.Get(), scaled_tab.Get(),
                g_app.settings.active_tab == DataTab::Incoming ? accent.Get() : text.Get(),
                localizer.Text("overlay.tab.incoming", "Taken"), incoming);
            DrawTextLine(target.Get(), scaled_tab.Get(),
                g_app.settings.active_tab == DataTab::HealingDone ? accent.Get() : text.Get(),
                localizer.Text("overlay.tab.healing_done", "Healing"), healing_done);
            DrawTextLine(target.Get(), scaled_tab.Get(),
                g_app.settings.active_tab == DataTab::HealingReceived ? accent.Get() : text.Get(),
                localizer.Text("overlay.tab.healing_received", "Healed"), healing_received);
            for (const auto& tab : {outgoing, incoming, healing_done, healing_received}) {
                target->DrawLine(D2D1::Point2F(tab.right, tab.top),
                    D2D1::Point2F(tab.right, tab.bottom), separator.Get(), scale);
            }
            add_target(outgoing, UiAction::OutgoingTab);
            add_target(incoming, UiAction::IncomingTab);
            add_target(healing_done, UiAction::HealingDoneTab);
            add_target(healing_received, UiAction::HealingReceivedTab);
        }
        nte::combat_stats::overlay::FooterData footer_data;
        if (snapshot) {
            footer_data.live_players = snapshot->combat.players.size();
            footer_data.live_dps = snapshot->combat.encounter_dps;
        }
        footer_data.history_count = HistoryEncounterCount(history);
        if (history_detail) {
            footer_data.history_players = history_detail->players.size();
            footer_data.history_dps = history_detail->summary.encounter_dps;
        }
        const auto footer_page = g_app.page == ViewPage::HistoryList
            ? nte::combat_stats::overlay::FooterPage::HistoryList
            : (g_app.page == ViewPage::HistoryDetail
                ? nte::combat_stats::overlay::FooterPage::HistoryDetail
                : nte::combat_stats::overlay::FooterPage::Live);
        const std::wstring footer_status =
            nte::combat_stats::overlay::FooterSummary(footer_page, footer_data);
        DrawTextLine(target.Get(), scaled_footer.Get(), text.Get(), footer_status,
            to_d2d_rect(footer_layout.summary));

        if (g_app.page == ViewPage::HistoryList) {
            draw_cells({localizer.Text("overlay.col.account_time", "Account / Time"), localizer.Text("overlay.col.duration", "Duration"), localizer.Text("overlay.col.outgoing", "Damage"), localizer.Text("overlay.col.taken", "Taken"), localizer.Text("overlay.col.dps", "DPS"), localizer.Text("overlay.col.end_reason", "End reason"), localizer.Text("overlay.col.quality", "Quality")},
                {1.8F, 0.7F, 0.9F, 0.9F, 0.9F, 1.2F, 0.7F},
                D2D1::RectF(0, top, layout.width, top + header_height),
                scaled_small.Get(), muted.Get(), true);
            top += header_height;
            if (!g_app.history_expansion_initialized && !history.empty()) {
                g_app.expanded_history_accounts.insert(
                    nte::account::DirectoryName(history.front().account));
                g_app.history_expansion_initialized = true;
            }
            g_app.displayed_history =
                nte::combat_stats::overlay::FlattenHistoryGroups(
                    history, g_app.expanded_history_accounts);
            const std::size_t visible = static_cast<std::size_t>(std::max(
                0.0F, content_bottom - top) / row_height);
            g_app.maximum_scroll = g_app.displayed_history.size() > visible
                ? g_app.displayed_history.size() - visible : 0;
            g_app.scroll_offset = (std::min)(g_app.scroll_offset, g_app.maximum_scroll);
            const std::size_t count = (std::min)(visible,
                g_app.displayed_history.size() - (std::min)(g_app.scroll_offset,
                    g_app.displayed_history.size()));
            for (std::size_t row{}; row < count; ++row) {
                const std::size_t index = g_app.scroll_offset + row;
                const auto& item = g_app.displayed_history[index];
                const D2D1_RECT_F rect = D2D1::RectF(0, top + row * row_height,
                    layout.width, top + (row + 1) * row_height);
                const std::size_t target_index = g_app.hit_targets.size();
                if (g_app.hovered_target == target_index) target->FillRectangle(rect, hover.Get());
                if (item.kind ==
                    nte::combat_stats::overlay::HistoryListRow::Kind::Account) {
                    const bool expanded = g_app.expanded_history_accounts.contains(
                        nte::account::DirectoryName(item.account));
                    draw_cells({std::wstring(expanded ? L"▾  " : L"▸  ") +
                            AccountLabel(item.account),
                        Substitute(
                            localizer.Text("overlay.list.count", "{0} encounters"),
                            std::to_wstring(item.encounter_count)), L"", L"", L"", L"", L""},
                        {1.8F, 0.7F, 0.9F, 0.9F, 0.9F, 1.2F, 0.7F}, rect,
                        scaled_regular.Get(), accent.Get());
                } else if (item.encounter) {
                    const auto& encounter = *item.encounter;
                    draw_cells({L"    " + TimeText(encounter.started_utc_ms),
                        std::format(L"{:.1f}s", encounter.duration_seconds),
                        CompactNumber(static_cast<double>(encounter.outgoing_damage)),
                        CompactNumber(static_cast<double>(encounter.incoming_damage)),
                        CompactNumber(encounter.encounter_dps),
                        Utf16(encounter.end_reason),
                        encounter.complete ? localizer.Text("overlay.quality.complete", "Complete") : localizer.Text("overlay.quality.missing", "Missing")},
                        {1.8F, 0.7F, 0.9F, 0.9F, 0.9F, 1.2F, 0.7F}, rect,
                        scaled_regular.Get(),
                        encounter.complete ? text.Get() : warning.Get());
                }
                add_target(rect, UiAction::SelectRow, index);
            }
            if (g_app.displayed_history.empty()) {
                DrawTextLine(target.Get(), scaled_regular.Get(), muted.Get(),
                    history_error.empty() ? localizer.Text("overlay.history.empty", "No completed encounters") : Utf16(history_error),
                    D2D1::RectF(12 * scale, top + 10 * scale,
                        layout.width - 12 * scale, content_bottom));
            }
        } else {
            std::vector<PlayerRow> players;
            std::vector<nte::combat_stats::overlay::CombatEventDetail> events;
            std::int64_t encounter_start_utc{};
            if (g_app.page == ViewPage::HistoryDetail) {
                if (history_detail && history_detail->summary.account ==
                        g_app.requested_account &&
                    history_detail->summary.report_id ==
                        g_app.requested_report_id &&
                    history_detail->summary.encounter_id == g_app.requested_encounter_id) {
                    players = history_detail->players;
                    events = history_detail->events;
                    encounter_start_utc = history_detail->summary.started_utc_ms;
                }
            } else if (snapshot) {
                players = snapshot->combat.players;
                events = MergeLiveEvents(*snapshot, std::move(live_events));
                encounter_start_utc = snapshot->sent_utc_ms - static_cast<std::int64_t>(
                    snapshot->combat.duration_seconds * 1000.0);
            }
            if (g_app.drill != DrillLevel::Players ||
                g_app.page == ViewPage::HistoryDetail) {
                const D2D1_RECT_F back = D2D1::RectF(
                    0, top, 58 * scale, top + header_height);
                DrawTextLine(target.Get(), scaled_regular.Get(), text.Get(), localizer.Text("overlay.nav.back", "‹ Back"), back);
                add_target(back, UiAction::Back);
                std::wstring breadcrumb = g_app.drill == DrillLevel::Players
                    ? localizer.Text("overlay.title.history", "Encounter history") : localizer.Text("overlay.col.character_source", "Character / Source");
                if (g_app.drill == DrillLevel::Events) breadcrumb += L" / " + localizer.Text("overlay.drill.hits", "hits");
                DrawTextLine(target.Get(), scaled_small.Get(), muted.Get(), breadcrumb,
                    D2D1::RectF(60 * scale, top, layout.width, top + header_height));
                top += header_height;
            }
            std::size_t total_rows{};
            std::vector<std::wstring> outgoing_headers;
            std::vector<float> outgoing_weights;
            if (g_app.drill == DrillLevel::Players) {
                if (g_app.settings.active_tab == DataTab::Outgoing) {
                    players = SortPlayers(players, g_app.settings.sort_column,
                        g_app.settings.descending);
                } else if (g_app.settings.active_tab == DataTab::HealingDone ||
                           g_app.settings.active_tab == DataTab::HealingReceived) {
                    players = nte::combat_stats::overlay::SortHealingPlayers(
                        players,
                        g_app.settings.active_tab == DataTab::HealingDone
                            ? nte::combat_stats::overlay::HealingDirection::Done
                            : nte::combat_stats::overlay::HealingDirection::Received);
                } else {
                    std::ranges::sort(players, [](const auto& left, const auto& right) {
                        if (left.incoming_damage != right.incoming_damage) {
                            return left.incoming_damage > right.incoming_damage;
                        }
                        return left.label < right.label;
                    });
                }
                g_app.displayed_players = std::move(players);
                total_rows = g_app.displayed_players.size();
                for (const auto& column :
                     nte::combat_stats::overlay::OutgoingSummaryColumns()) {
                    outgoing_headers.emplace_back(localizer.Text(column.key, Narrow(column.fallback)));
                    outgoing_weights.push_back(column.weight);
                }
                draw_cells(g_app.settings.active_tab == DataTab::Outgoing
                        ? outgoing_headers
                        : (g_app.settings.active_tab == DataTab::HealingDone ||
                           g_app.settings.active_tab == DataTab::HealingReceived)
                            ? std::vector<std::wstring>{localizer.Text("overlay.col.character", "Character"), localizer.Text("overlay.col.healing", "Healing"), localizer.Text("overlay.col.hps", "HPS"), localizer.Text("overlay.col.share", "Share"), localizer.Text("overlay.col.count", "Count"), localizer.Text("overlay.col.average", "Avg"), localizer.Text("overlay.col.max", "Max")}
                            : std::vector<std::wstring>{localizer.Text("overlay.col.character", "Character"), localizer.Text("overlay.col.taken", "Taken"), localizer.Text("overlay.col.share", "Share"), localizer.Text("overlay.col.count", "Count"), localizer.Text("overlay.col.average", "Avg"), localizer.Text("overlay.col.max", "Max")},
                    g_app.settings.active_tab == DataTab::Outgoing
                        ? outgoing_weights
                        : (g_app.settings.active_tab == DataTab::HealingDone ||
                           g_app.settings.active_tab == DataTab::HealingReceived)
                            ? std::vector<float>{1.8F, 1.0F, 0.8F, 0.8F, 0.7F, 0.9F, 0.9F}
                            : std::vector<float>{2.0F, 1.1F, 0.9F, 1.0F, 0.9F, 1.1F},
                    D2D1::RectF(0, top, layout.width, top + header_height),
                    scaled_small.Get(), muted.Get(), true);
            } else {
                const auto selected = std::ranges::find_if(players, [&](const auto& player) {
                    return !g_app.navigation.player_key.empty()
                        ? PlayerKey(player) == g_app.navigation.player_key
                        : player.handle == g_app.selected_player;
                });
                if (selected != players.end()) {
                    g_app.selected_player = selected->handle;
                    events = CanonicalPlayerEvents(std::move(events), *selected);
                }
                const bool healing =
                    g_app.settings.active_tab == DataTab::HealingDone ||
                    g_app.settings.active_tab == DataTab::HealingReceived;
                const auto direction = g_app.settings.active_tab == DataTab::Outgoing ||
                        g_app.settings.active_tab == DataTab::HealingDone
                    ? nte::combat_stats::overlay::DetailDirection::Outgoing
                    : nte::combat_stats::overlay::DetailDirection::Incoming;
                const auto event_kind = healing
                    ? nte::combat_stats::CombatEventKind::Heal
                    : nte::combat_stats::CombatEventKind::Damage;
                auto grouped_sources = nte::combat_stats::overlay::GroupEventSources(
                    events, g_app.selected_player, direction, event_kind);
                if (g_app.drill == DrillLevel::Events) {
                    const auto rebound = std::ranges::find_if(
                        grouped_sources, [&](const auto& source) {
                            return SourceKey(source) == g_app.navigation.source_key;
                        });
                    // Live disk events may lag the snapshot. An empty source list
                    // is a loading state, not proof that the selection vanished.
                    if (rebound == grouped_sources.end() && !grouped_sources.empty()) {
                        g_app.drill = DrillLevel::Sources;
                        g_app.navigation.drill =
                            nte::combat_stats::overlay::DrillLevel::Sources;
                        g_app.navigation.source_key.clear();
                        g_app.scroll_offset = 0;
                    } else {
                        g_app.selected_source = *rebound;
                    }
                }
                if (g_app.drill == DrillLevel::Sources) {
                    g_app.displayed_sources = std::move(grouped_sources);
                    total_rows = g_app.displayed_sources.size();
                    draw_cells({localizer.Text("overlay.col.source", "Source"), healing ? localizer.Text("overlay.col.healing", "Healing") : localizer.Text("overlay.col.damage", "Damage"), localizer.Text("overlay.col.count", "Count"), localizer.Text("overlay.col.average", "Avg"), localizer.Text("overlay.col.max", "Max")},
                        {2.5F, 1.1F, 0.8F, 1.0F, 1.0F},
                        D2D1::RectF(0, top, layout.width, top + header_height),
                        scaled_small.Get(), muted.Get(), true);
                } else {
                    g_app.displayed_events = nte::combat_stats::overlay::FilterEvents(
                        events, g_app.selected_player, direction, event_kind,
                        g_app.selected_source);
                    total_rows = g_app.displayed_events.size();
                    draw_cells({localizer.Text("overlay.col.relative_time", "Relative / Received"), healing ? localizer.Text("overlay.col.healing", "Healing") : localizer.Text("overlay.col.final", "Final"),
                            healing ? localizer.Text("overlay.col.base_raw", "Base / Raw") : localizer.Text("overlay.col.base_display", "Base / Display"),
                            healing ? L"" : localizer.Text("overlay.col.flags", "Flags"),
                            direction == nte::combat_stats::overlay::DetailDirection::Outgoing
                                ? localizer.Text("overlay.col.target", "Target") : localizer.Text("overlay.col.source_character", "Source character")},
                        {2.3F, 1.0F, 1.4F, 0.8F, 1.8F},
                        D2D1::RectF(0, top, layout.width, top + header_height),
                        scaled_small.Get(), muted.Get(), true);
                }
            }
            top += header_height;
            const std::size_t visible = static_cast<std::size_t>(std::max(
                0.0F, content_bottom - top) / row_height);
            g_app.maximum_scroll = total_rows > visible ? total_rows - visible : 0;
            g_app.scroll_offset = (std::min)(g_app.scroll_offset, g_app.maximum_scroll);
            const std::size_t count = (std::min)(visible,
                total_rows - (std::min)(g_app.scroll_offset, total_rows));
            std::int64_t maximum_player_damage{};
            if (g_app.drill == DrillLevel::Players) {
                for (const auto& player : g_app.displayed_players) {
                    const std::int64_t amount =
                        g_app.settings.active_tab == DataTab::Outgoing ? player.damage :
                        g_app.settings.active_tab == DataTab::Incoming ? player.incoming_damage :
                        g_app.settings.active_tab == DataTab::HealingDone ? player.healing_done :
                        player.healing_received;
                    maximum_player_damage = (std::max)(maximum_player_damage,
                        amount);
                }
            }
            for (std::size_t row{}; row < count; ++row) {
                const std::size_t index = g_app.scroll_offset + row;
                const D2D1_RECT_F rect = D2D1::RectF(0, top + row * row_height,
                    layout.width, top + (row + 1) * row_height);
                const std::size_t target_index = g_app.hit_targets.size();
                std::optional<std::uint32_t> player_color;
                if (g_app.drill == DrillLevel::Players) {
                    const auto& player = g_app.displayed_players[index];
                    player_color = nte::combat_stats::overlay::ResolvePlayerDisplayColor(
                        player, g_app.attribute_colors);
                    const std::int64_t damage =
                        g_app.settings.active_tab == DataTab::Outgoing ? player.damage :
                        g_app.settings.active_tab == DataTab::Incoming ? player.incoming_damage :
                        g_app.settings.active_tab == DataTab::HealingDone ? player.healing_done :
                        player.healing_received;
                    const float fraction = static_cast<float>(
                        nte::combat_stats::overlay::PlayerBarFraction(
                            damage, maximum_player_damage));
                    ComPtr<ID2D1SolidColorBrush> player_bar;
                    target->CreateSolidColorBrush(D2D1::ColorF(
                        *player_color,
                        0.78F), &player_bar);
                    target->FillRectangle(D2D1::RectF(rect.left, rect.top,
                        rect.left + (rect.right - rect.left) * fraction, rect.bottom),
                        player_bar.Get());
                }
                if (g_app.hovered_target == target_index) target->FillRectangle(rect, hover.Get());
                if (g_app.drill == DrillLevel::Players) {
                    const auto& player = g_app.displayed_players[index];
                    const bool outgoing = g_app.settings.active_tab == DataTab::Outgoing;
                    const bool healing_done =
                        g_app.settings.active_tab == DataTab::HealingDone;
                    const bool healing_received =
                        g_app.settings.active_tab == DataTab::HealingReceived;
                    const bool healing = healing_done || healing_received;
                    const double average = outgoing
                        ? (player.positive_hits == 0 ? 0.0 :
                            static_cast<double>(player.damage) / player.positive_hits)
                        : healing
                            ? nte::combat_stats::overlay::HealingAverage(
                                player, healing_done
                                    ? nte::combat_stats::overlay::HealingDirection::Done
                                    : nte::combat_stats::overlay::HealingDirection::Received)
                        : (player.incoming_positive_hits == 0 ? 0.0 :
                            static_cast<double>(player.incoming_damage) /
                                player.incoming_positive_hits);
                    draw_cells(outgoing
                        ? std::vector<std::wstring>{Utf16(player.display_name.empty()
                                ? player.label : player.display_name),
                            std::to_wstring(player.damage),
                            std::format(L"{:.0f}", player.encounter_dps),
                            std::format(L"{}%", nte::combat_stats::overlay::RoundedPercent(
                                player.damage_share)),
                            std::format(L"{:.1f}%", PlayerCriticalRate(player)),
                            std::format(L"{} {}", player.maximum_hit,
                                Utf16(player.maximum_source_display_name.empty()
                                    ? player.maximum_source_name
                                    : player.maximum_source_display_name))}
                        : healing
                            ? std::vector<std::wstring>{Utf16(player.display_name.empty()
                                    ? player.label : player.display_name),
                                CompactNumber(static_cast<double>(healing_done
                                    ? player.healing_done : player.healing_received)),
                                CompactNumber(healing_done
                                    ? player.encounter_hps : player.healing_received_hps),
                                std::format(L"{:.1f}%", healing_done
                                    ? player.healing_share : player.healing_received_share),
                                std::to_wstring(healing_done
                                    ? player.healing_done_events : player.healing_received_events),
                                CompactNumber(average),
                                CompactNumber(static_cast<double>(healing_done
                                    ? player.maximum_heal : player.maximum_heal_received))}
                        : std::vector<std::wstring>{Utf16(player.display_name.empty()
                                ? player.label : player.display_name),
                            CompactNumber(static_cast<double>(player.incoming_damage)),
                            std::format(L"{:.1f}%", player.incoming_share),
                            std::to_wstring(player.incoming_positive_hits),
                            CompactNumber(average),
                            CompactNumber(static_cast<double>(player.maximum_incoming_hit))},
                        outgoing
                            ? outgoing_weights
                            : healing
                                ? std::vector<float>{1.8F, 1.0F, 0.8F, 0.8F, 0.7F, 0.9F, 0.9F}
                                : std::vector<float>{2.0F, 1.1F, 0.9F, 1.0F, 0.9F, 1.1F}, rect,
                        scaled_regular.Get(), text.Get(),
                        false,
                        nte::combat_stats::overlay::PlayerNameInset(scale));
                    ComPtr<ID2D1SolidColorBrush> glyph;
                    target->CreateSolidColorBrush(
                        D2D1::ColorF(*player_color, 1.0F), &glyph);
                    const auto glyph_rect =
                        nte::combat_stats::overlay::CalculatePlayerGlyphRect(
                            {rect.left, rect.top, rect.right, rect.bottom}, scale);
                    DrawTextLine(target.Get(), scaled_bold.Get(), glyph.Get(), L"◇",
                        to_d2d_rect(glyph_rect));
                    add_target(rect, UiAction::SelectRow, index);
                } else if (g_app.drill == DrillLevel::Sources) {
                    const auto& source = g_app.displayed_sources[index];
                    draw_cells({Utf16(source.label),
                        CompactNumber(static_cast<double>(source.amount)),
                        std::to_wstring(source.count),
                        CompactNumber(source.count == 0 ? 0.0 :
                            static_cast<double>(source.amount) / source.count),
                        CompactNumber(static_cast<double>(source.maximum))},
                        {2.5F, 1.1F, 0.8F, 1.0F, 1.0F}, rect,
                        scaled_regular.Get(), text.Get());
                    add_target(rect, UiAction::SelectRow, index);
                } else {
                    const auto& event = g_app.displayed_events[index];
                    std::wstring flags;
                    if (event.kind == nte::combat_stats::CombatEventKind::Damage &&
                        (event.flags & nte::combat_stats::kDamageFlagCritical) != 0)
                        flags += localizer.Text("overlay.flag.critical", "C");
                    if (event.kind == nte::combat_stats::CombatEventKind::Damage &&
                        (event.flags & nte::combat_stats::kDamageFlagHeadHit) != 0)
                        flags += localizer.Text("overlay.flag.head", "H");
                    if (event.kind == nte::combat_stats::CombatEventKind::Damage &&
                        (event.flags & nte::combat_stats::kDamageFlagWeakUnbalance) != 0)
                        flags += localizer.Text("overlay.flag.weak", "W");
                    const bool critical =
                        event.kind == nte::combat_stats::CombatEventKind::Damage &&
                        (event.flags & nte::combat_stats::kDamageFlagCritical) != 0;
                    const double relative = encounter_start_utc == 0 ? 0.0 :
                        static_cast<double>(event.received_utc_ms - encounter_start_utc) / 1000.0;
                    draw_cells({EventTimeText(event.received_utc_ms,
                            std::max(0.0, relative)),
                        CompactNumber(static_cast<double>(event.final_value)),
                        std::format(L"{}/{}", event.basic_value, event.value),
                        flags, Utf16((g_app.settings.active_tab == DataTab::Outgoing ||
                                     g_app.settings.active_tab == DataTab::HealingDone)
                            ? (event.victim_display_name.empty()
                                ? event.victim_name : event.victim_display_name)
                            : (event.attacker_display_name.empty()
                                ? event.attacker_name : event.attacker_display_name))},
                        {2.3F, 1.0F, 1.4F, 0.8F, 1.8F}, rect,
                        scaled_regular.Get(), critical ? accent.Get() : text.Get());
                }
            }
            if (total_rows == 0) {
                const std::wstring message = g_app.page == ViewPage::HistoryDetail &&
                        !history_detail ? localizer.Text("overlay.history.loading", "Loading encounter history…") :
                    (snapshot ? localizer.Text("overlay.state.no_data", "No data") : Utf16(status));
                DrawTextLine(target.Get(), scaled_regular.Get(), muted.Get(), message,
                    D2D1::RectF(12 * scale, top + 10 * scale,
                        layout.width - 12 * scale, content_bottom));
            }
        }
        const D2D1_RECT_F grip = to_d2d_rect(footer_layout.resize_grip);
        const float inset = 2 * scale;
        for (int line{}; line < 3; ++line) {
            const float offset = static_cast<float>(line) * 3 * scale;
            target->DrawLine(D2D1::Point2F(grip.left + inset + offset,
                    grip.bottom - inset),
                D2D1::Point2F(grip.right - inset,
                    grip.top + inset + offset), muted.Get(), scale);
        }
    }
    if (FAILED(target->EndDraw())) return;

    WICRect lock_rect{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
    ComPtr<IWICBitmapLock> bitmap_lock;
    if (FAILED(bitmap->Lock(&lock_rect, WICBitmapLockRead, &bitmap_lock))) return;
    UINT stride{};
    UINT bytes{};
    BYTE* pixels{};
    bitmap_lock->GetStride(&stride);
    bitmap_lock->GetDataPointer(&bytes, &pixels);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* dib_pixels{};
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &dib_pixels, nullptr, 0);
    if (dib && dib_pixels) {
        for (UINT row = 0; row < height; ++row) {
            memcpy(static_cast<BYTE*>(dib_pixels) + row * width * 4,
                pixels + row * stride, width * 4);
        }
        const HGDIOBJ old = SelectObject(memory, dib);
        RECT window_rect{};
        GetWindowRect(g_app.window, &window_rect);
        POINT destination{window_rect.left, window_rect.top};
        POINT source{};
        SIZE size{static_cast<LONG>(width), static_cast<LONG>(height)};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(g_app.window, screen, &destination, &size,
            memory, &source, 0, &blend, ULW_ALPHA);
        SelectObject(memory, old);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

void SetConnectionStatus(std::string status, const bool connected) {
    {
        std::scoped_lock lock(g_app.snapshot_mutex);
        g_app.connection_status = std::move(status);
        g_app.connected = connected;
    }
    if (g_app.window) PostMessageW(g_app.window, kSnapshotMessage, 0, 0);
}

struct HttpHandle {
    HINTERNET value{};
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
    HttpHandle() = default;
    explicit HttpHandle(HINTERNET handle) : value(handle) {}
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
};

void ReceiveSnapshots(const std::stop_token stop) {
    while (!stop.stop_requested()) {
        SetConnectionStatus(Narrow(localizer.Text("overlay.conn.connecting", "Connecting to the Runtime...")), false);
        HttpHandle session(WinHttpOpen(nte::build::kOverlayUserAgent,
            WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0));
        if (session.value) WinHttpSetTimeouts(session.value, 1000, 1000, 1000, 1000);
        HttpHandle connection(session.value ? WinHttpConnect(
            session.value, L"127.0.0.1", g_app.port, 0) : nullptr);
        HttpHandle request(connection.value ? WinHttpOpenRequest(connection.value, L"GET", L"/",
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr);
        {
            std::scoped_lock lock(g_app.socket_mutex);
            g_app.active_request = request.value;
        }
        const bool request_ready = request.value && WinHttpSetOption(request.value,
                WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
            WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request.value, nullptr);
        {
            std::scoped_lock lock(g_app.socket_mutex);
            if (g_app.active_request == request.value) g_app.active_request = nullptr;
            if (g_app.cancelled_request == request.value) {
                request.value = nullptr;
                g_app.cancelled_request = nullptr;
            }
        }
        if (!request_ready) {
            SetConnectionStatus(Narrow(localizer.Text("overlay.conn.not_connected", "Runtime WebSocket is not connected")), false);
            for (int i = 0; i < 20 && !stop.stop_requested(); ++i) Sleep(100);
            continue;
        }
        HttpHandle socket(WinHttpWebSocketCompleteUpgrade(request.value, 0));
        if (!socket.value) {
            SetConnectionStatus(Narrow(localizer.Text("overlay.conn.handshake_failed", "WebSocket handshake failed")), false);
            Sleep(1000);
            continue;
        }
        SetConnectionStatus(Narrow(localizer.Text("overlay.conn.awaiting_data", "Connected, waiting for NTE data")), true);
        {
            std::scoped_lock lock(g_app.socket_mutex);
            g_app.active_socket = socket.value;
        }
        std::vector<char> message;
        message.reserve(8192);
        bool reconnect{};
        while (!stop.stop_requested() && !reconnect) {
            std::array<char, 8192> chunk{};
            DWORD read{};
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            const DWORD result = WinHttpWebSocketReceive(socket.value, chunk.data(),
                static_cast<DWORD>(chunk.size()), &read, &type);
            if (result != ERROR_SUCCESS || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
            if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE &&
                type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
                message.clear();
                continue;
            }
            if (message.size() + read > kMaximumMessage) {
                message.clear();
                reconnect = true;
                continue;
            }
            message.insert(message.end(), chunk.data(), chunk.data() + read);
            if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) continue;
            std::string error;
            auto parsed = nte::combat_stats::ParseOverlaySnapshot(
                std::string_view(message.data(), message.size()), error);
            message.clear();
            if (!parsed) continue;
            if (parsed->producer_stopping) {
                {
                    std::scoped_lock lock(g_app.socket_mutex);
                    if (g_app.active_socket == socket.value) g_app.active_socket = nullptr;
                }
                PostMessageW(g_app.window, WM_CLOSE, 0, 0);
                return;
            }
            bool redraw = true;
            {
                std::scoped_lock lock(g_app.snapshot_mutex);
                if (g_app.snapshot && parsed->producer_instance_id ==
                        g_app.snapshot->producer_instance_id &&
                    parsed->snapshot_sequence <= g_app.snapshot->snapshot_sequence) continue;
                if (g_app.snapshot && parsed->combat.state == EncounterState::Idle &&
                    g_app.snapshot->combat.state == EncounterState::Idle &&
                    parsed->capture_available == g_app.snapshot->capture_available &&
                    parsed->capture_status == g_app.snapshot->capture_status &&
                    parsed->latest_event_sequence ==
                        g_app.snapshot->latest_event_sequence) {
                    redraw = false;
                }
                g_app.snapshot.emplace(std::move(*parsed));
                g_app.last_snapshot = std::chrono::steady_clock::now();
                // The locale arrives with each snapshot; reload only when it
                // actually changes, since the Host fixes it per process.
                if (!g_app.executable_root.empty() && g_app.text_locale != g_app.snapshot->locale) {
                    g_app.text_locale = g_app.snapshot->locale;
                    g_app.text.Load(g_app.executable_root, g_app.text_locale);
                }
                g_app.connection_status =
                    Narrow(localizer.Text("overlay.state.connected", "Connected"));
            }
            if (redraw) PostMessageW(g_app.window, kSnapshotMessage, 0, 0);
        }
        {
            std::scoped_lock lock(g_app.socket_mutex);
            if (g_app.active_socket == socket.value) g_app.active_socket = nullptr;
        }
        WinHttpWebSocketClose(socket.value, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
            nullptr, 0);
        SetConnectionStatus(Narrow(localizer.Text("overlay.conn.reconnecting", "Disconnected, reconnecting...")), false);
        for (int i = 0; i < 10 && !stop.stop_requested(); ++i) Sleep(100);
    }
}

void LoadHistoryData(const std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::string settings_error;
        const auto plugin_settings = nte::settings::LoadPluginSettings(
            nte::settings::PluginSettingsPath(),
            nte::damage_log::DefaultLogRoot(), settings_error);
        nte::combat_stats::overlay::CombatLogReader reader(
            plugin_settings.log_root);
        std::string history_error;
        auto history = reader.History(history_error);
        std::string live_report;
        std::string live_encounter;
        std::string requested_report;
        std::string requested_encounter;
        nte::account::AccountIdentity live_account;
        nte::account::AccountIdentity requested_account;
        bool load_detail{};
        {
            std::scoped_lock lock(g_app.snapshot_mutex);
            if (g_app.snapshot) {
                live_report = g_app.snapshot->report_id;
                live_encounter = g_app.snapshot->encounter_id;
                live_account = g_app.snapshot->account;
            }
            requested_report = g_app.requested_report_id;
            requested_encounter = g_app.requested_encounter_id;
            requested_account = g_app.requested_account;
            load_detail = !requested_report.empty() && !requested_encounter.empty() &&
                (!g_app.history_detail ||
                 g_app.history_detail->summary.account != requested_account ||
                 g_app.history_detail->summary.report_id != requested_report ||
                 g_app.history_detail->summary.encounter_id != requested_encounter);
        }
        std::string live_error;
        std::vector<nte::combat_stats::overlay::CombatEventDetail> live_events;
        if (!live_report.empty() && !live_encounter.empty()) {
            live_events = reader.Events(
                live_account, live_report, live_encounter, live_error);
        }
        std::string detail_error;
        std::optional<nte::combat_stats::overlay::EncounterDetail> detail;
        if (load_detail) {
            detail = reader.Encounter(requested_account,
                requested_report, requested_encounter, detail_error);
        }
        bool definitive_detail_failure{};
        {
            std::scoped_lock lock(g_app.snapshot_mutex);
            if (history_error.empty() || !history.empty()) {
                g_app.history = std::move(history);
            }
            g_app.history_error = !detail_error.empty() ? detail_error :
                (!live_error.empty() ? live_error : history_error);
            if (live_error.empty() && g_app.snapshot &&
                g_app.snapshot->account == live_account &&
                g_app.snapshot->report_id == live_report &&
                g_app.snapshot->encounter_id == live_encounter) {
                g_app.live_events = std::move(live_events);
                g_app.loaded_live_report_id = std::move(live_report);
                g_app.loaded_live_encounter_id = std::move(live_encounter);
                g_app.loaded_live_account = std::move(live_account);
            }
            if (detail_error.empty() && detail &&
                g_app.requested_account == requested_account &&
                g_app.requested_report_id == requested_report &&
                g_app.requested_encounter_id == requested_encounter) {
                g_app.history_detail = std::move(detail);
            } else if (load_detail &&
                (detail_error == "encounter not found" ||
                 detail_error == "invalid report or encounter id") &&
                g_app.requested_report_id == requested_report &&
                g_app.requested_encounter_id == requested_encounter) {
                g_app.history_detail.reset();
                g_app.requested_account = nte::account::Unknown();
                g_app.requested_report_id.clear();
                g_app.requested_encounter_id.clear();
                definitive_detail_failure = true;
            }
        }
        if (g_app.window != nullptr) {
            PostMessageW(g_app.window, kHistoryRefreshMessage,
                definitive_detail_failure ? 1 : 0, 0);
        }
        for (int count{}; count < 10 && !stop.stop_requested(); ++count) Sleep(100);
    }
}

nte::combat_stats::overlay::CombatEventDetail ConvertRecentEvent(
    const nte::combat_stats::RecentEvent& event,
    const std::string& encounter_id) {
    nte::combat_stats::overlay::CombatEventDetail result;
    static_cast<nte::combat_stats::CombatEventData&>(result) =
        static_cast<const nte::combat_stats::CombatEventData&>(event);
    result.encounter_id = encounter_id;
    result.source = event.source_name;
    result.source_display_name = event.source_display_name;
    result.attacker_name = event.attacker_name;
    result.attacker_display_name = event.attacker_display_name;
    result.victim_name = event.victim_name;
    result.victim_display_name = event.victim_display_name;
    return result;
}

std::vector<nte::combat_stats::overlay::CombatEventDetail> MergeLiveEvents(
    const NteOverlaySnapshot& snapshot,
    std::vector<nte::combat_stats::overlay::CombatEventDetail> events) {
    for (const auto& recent : snapshot.combat.recent_events) {
        events.push_back(ConvertRecentEvent(recent, snapshot.encounter_id));
    }
    std::ranges::sort(events, {},
        &nte::combat_stats::overlay::CombatEventDetail::sequence);
    events.erase(std::unique(events.begin(), events.end(), [](const auto& left,
                     const auto& right) { return left.sequence == right.sequence; }),
        events.end());
    return events;
}

void WatchLifetime(const std::stop_token stop, const DWORD host_process_id) {
    HANDLE stopping = OpenEventW(SYNCHRONIZE, FALSE,
        nte::combat_stats::overlay::kPluginStoppingEventName);
    HANDLE host = host_process_id == 0 ? nullptr :
        OpenProcess(SYNCHRONIZE, FALSE, host_process_id);
    std::array<HANDLE, 2> handles{};
    DWORD count{};
    if (stopping != nullptr) handles[count++] = stopping;
    if (host != nullptr) handles[count++] = host;
    // The stop event is advisory and may be absent in older plugin builds. Only
    // a successfully opened host-process handle makes the stale timeout unnecessary.
    g_app.lifetime_guarded.store(host != nullptr);
    while (!stop.stop_requested() && count != 0) {
        const DWORD result = WaitForMultipleObjects(count, handles.data(), FALSE, 100);
        if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count) {
            PostMessageW(g_app.window, WM_CLOSE, 0, 0);
            break;
        }
        if (result == WAIT_FAILED) break;
    }
    if (host != nullptr) CloseHandle(host);
    if (stopping != nullptr) CloseHandle(stopping);
}

void StopBackgroundWork() {
    if (g_app.export_worker.joinable()) g_app.export_worker.request_stop();
    if (g_app.history_loader.joinable()) g_app.history_loader.request_stop();
    if (g_app.lifetime_watcher.joinable()) g_app.lifetime_watcher.request_stop();
    if (g_app.receiver.joinable()) g_app.receiver.request_stop();
    {
        std::scoped_lock lock(g_app.socket_mutex);
        if (g_app.active_request != nullptr) {
            g_app.cancelled_request = g_app.active_request;
            WinHttpCloseHandle(g_app.active_request);
            g_app.active_request = nullptr;
        }
        if (g_app.active_socket != nullptr) {
            WinHttpWebSocketShutdown(g_app.active_socket,
                WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        }
    }
    if (g_app.history_loader.joinable()) {
        g_app.history_loader.join();
    }
    if (g_app.lifetime_watcher.joinable()) {
        g_app.lifetime_watcher.join();
    }
    if (g_app.receiver.joinable()) {
        g_app.receiver.join();
    }
    if (g_app.export_worker.joinable()) {
        g_app.export_worker.join();
    }
}

void ToggleLock() {
    g_app.settings.locked = !g_app.settings.locked;
    ApplyWindowMode();
    SaveSettings();
    RenderOverlay();
}

void ToggleCollapsed() {
    RECT rect{};
    GetWindowRect(g_app.window, &rect);
    g_app.settings.collapsed = !g_app.settings.collapsed;
    const float dpi = static_cast<float>(GetDpiForWindow(g_app.window)) / 96.0F;
    const float title_height =
        nte::combat_stats::overlay::OverlayReferenceMetrics().title_height;
    const int height = g_app.settings.collapsed
        ? static_cast<int>(std::round(title_height * dpi * g_app.settings.ui_scale))
        : ScaleWindowExtent(1, g_app.settings.height,
            dpi, g_app.settings.ui_scale).height;
    SetWindowPos(g_app.window, nullptr, 0, 0, rect.right - rect.left, height,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SaveSettings();
    RenderOverlay();
}

std::optional<std::filesystem::path> SelectExportDirectory(std::string& error) {
    error.clear();
    ComPtr<IFileOpenDialog> dialog;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(result)) {
        error = Narrow(localizer.Text("overlay.export.err_open", "Cannot open the folder picker"));
        return std::nullopt;
    }
    DWORD options{};
    if (FAILED(dialog->GetOptions(&options)) || FAILED(dialog->SetOptions(
            options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST))) {
        error = Narrow(localizer.Text("overlay.export.err_config", "Cannot configure the folder picker"));
        return std::nullopt;
    }
    dialog->SetTitle(localizer.Text("overlay.dialog.export_dir", "Select NTE combat export directory").c_str());
    result = dialog->Show(g_app.window);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
    if (FAILED(result)) {
        error = Narrow(localizer.Text("overlay.export.err_select", "Selecting the export folder failed"));
        return std::nullopt;
    }
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) {
        error = Narrow(localizer.Text("overlay.export.err_read", "Cannot read the selected export folder"));
        return std::nullopt;
    }
    PWSTR selected{};
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected)) ||
        selected == nullptr) {
        error = Narrow(localizer.Text("overlay.export.err_not_fs", "The selected export folder is not a filesystem path"));
        return std::nullopt;
    }
    std::filesystem::path path(selected);
    CoTaskMemFree(selected);
    return path;
}

struct ExportRequest {
    std::filesystem::path log_root;
    std::filesystem::path export_root;
    std::string report_id;
    std::string encounter_id;
    nte::account::AccountIdentity account;
};

bool CurrentExportTarget(
    nte::account::AccountIdentity& account,
    std::string& report_id,
    std::string& encounter_id) {
    std::scoped_lock lock(g_app.snapshot_mutex);
    std::optional<EncounterState> live_state;
    if (g_app.snapshot) live_state = g_app.snapshot->combat.state;
    const bool history_ready = g_app.history_detail.has_value();
    if (!nte::combat_stats::overlay::CanExportEncounter(
            static_cast<nte::combat_stats::overlay::ViewPage>(g_app.page),
            live_state, history_ready) || g_app.export_in_progress) {
        return false;
    }
    if (g_app.page == ViewPage::Live) {
        account = g_app.snapshot->account;
        report_id = g_app.snapshot->report_id;
        encounter_id = g_app.snapshot->encounter_id;
    } else {
        account = g_app.history_detail->summary.account;
        report_id = g_app.history_detail->summary.report_id;
        encounter_id = g_app.history_detail->summary.encounter_id;
    }
    return !report_id.empty() && !encounter_id.empty();
}

void RunExport(const std::stop_token stop, ExportRequest request) {
    std::string error;
    nte::combat_stats::overlay::CombatLogReader reader(request.log_root);
    auto detail = reader.ExportEncounter(
        request.account, request.report_id, request.encounter_id, error, stop);
    if (detail && error.empty() && !stop.stop_requested()) {
        const auto exported_utc_ms = std::chrono::duration_cast<
            std::chrono::milliseconds>(std::chrono::system_clock::now()
                .time_since_epoch()).count();
        std::filesystem::path written;
        // Success is tracked by the return value, not by sniffing a translated
        // prefix: the message text now depends on the active catalog.
        bool exported = false;
        if (nte::combat_stats::overlay::WriteCombatExport(
                request.export_root, *detail, nte::build::kPluginVersion,
                exported_utc_ms, written, error, stop)) {
            exported = true;
            error = Substitute(
                Narrow(localizer.Text("overlay.export.done", "Export complete: {0}")),
                nte::settings::PathToUtf8(written));
        }
        if (stop.stop_requested()) return;
        {
            std::scoped_lock lock(g_app.snapshot_mutex);
            g_app.export_result_status = exported
                ? std::move(error)
                : Substitute(Narrow(localizer.Text("overlay.export.failed", "Export failed: {0}")), error);
            g_app.export_in_progress = false;
        }
        if (g_app.window != nullptr) {
            PostMessageW(g_app.window, kExportCompleteMessage, 0, 0);
        }
        return;
    } else if (error.empty() && !stop.stop_requested()) {
        error = Narrow(localizer.Text("overlay.export.err_none", "No completed encounter log was found"));
    }
}

void BeginExport() {
    nte::account::AccountIdentity account;
    std::string report_id;
    std::string encounter_id;
    if (!CurrentExportTarget(account, report_id, encounter_id)) return;

    std::string settings_error;
    auto settings = nte::settings::LoadPluginSettings(
        nte::settings::PluginSettingsPath(),
        nte::damage_log::DefaultLogRoot(), settings_error);
    if (!settings_error.empty()) {
        g_app.action_status = Substitute(Narrow(localizer.Text("overlay.export.failed", "Export failed: {0}")), settings_error);
        RenderOverlay();
        return;
    }
    if (!settings.export_root) {
        std::string selection_error;
        const auto selected = SelectExportDirectory(selection_error);
        if (!selected) {
            if (!selection_error.empty()) {
                g_app.action_status = Substitute(Narrow(localizer.Text("overlay.export.failed", "Export failed: {0}")), selection_error);
                RenderOverlay();
            }
            return;
        }
        std::string prepare_error;
        if (!nte::settings::PrepareLogRoot(*selected, prepare_error)) {
            g_app.action_status = Substitute(Narrow(localizer.Text("overlay.export.failed_dir", "Export failed: the export folder is unavailable: {0}")), prepare_error);
            RenderOverlay();
            return;
        }
        nte::settings::PluginSettings merged;
        if (!nte::settings::UpdatePluginExportRoot(
                nte::settings::PluginSettingsPath(), settings.log_root,
                selected, merged, settings_error)) {
            g_app.action_status = Substitute(Narrow(localizer.Text("overlay.export.failed_save", "Export failed: cannot save the export folder: {0}")), settings_error);
            RenderOverlay();
            return;
        }
        settings = std::move(merged);
    }
    {
        std::scoped_lock lock(g_app.snapshot_mutex);
        if (g_app.export_in_progress) return;
        g_app.export_in_progress = true;
    }
    if (g_app.export_worker.joinable()) g_app.export_worker.join();
    g_app.action_status = Narrow(localizer.Text("overlay.export.running", "Exporting the current encounter..."));
    RenderOverlay();
    g_app.export_worker = std::jthread(RunExport, ExportRequest{
        settings.log_root, *settings.export_root,
        std::move(report_id), std::move(encounter_id), std::move(account)});
}

void ShowContextMenu(const POINT screen) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g_app.settings.locked ? MF_CHECKED : 0),
        MenuToggleLock, (localizer.Text("overlay.menu.lock", "Lock / click-through") + L"\tCtrl+Shift+F12").c_str());
    AppendMenuW(menu, MF_STRING | (g_app.settings.collapsed ? MF_CHECKED : 0),
        MenuToggleCollapsed, localizer.Text("overlay.menu.collapse", "Collapse").c_str());
    AppendMenuW(menu, MF_STRING | (g_app.settings.topmost ? MF_CHECKED : 0),
        MenuToggleTopmost, localizer.Text("overlay.menu.topmost", "Always on top").c_str());
    AppendMenuW(menu, MF_STRING | (g_app.settings.hide_idle ? MF_CHECKED : 0),
        MenuToggleHideIdle, localizer.Text("overlay.menu.hide_idle", "Hide when idle").c_str());
    HMENU opacity = CreatePopupMenu();
    AppendMenuW(opacity, MF_STRING, MenuOpacity60, L"60%");
    AppendMenuW(opacity, MF_STRING, MenuOpacity75, L"75%");
    AppendMenuW(opacity, MF_STRING, MenuOpacity90, L"90%");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(opacity), localizer.Text("overlay.menu.opacity", "Opacity").c_str());
    HMENU scale = CreatePopupMenu();
    AppendMenuW(scale, MF_STRING, MenuScale100, L"100%");
    AppendMenuW(scale, MF_STRING, MenuScale125, L"125%");
    AppendMenuW(scale, MF_STRING, MenuScale150, L"150%");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(scale), localizer.Text("overlay.menu.ui_scale", "UI scale").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    std::string export_report;
    std::string export_encounter;
    nte::account::AccountIdentity export_account;
    const bool can_export = CurrentExportTarget(
        export_account, export_report, export_encounter);
    AppendMenuW(menu, MF_STRING | (can_export ? 0 : MF_GRAYED),
        MenuExportCurrent, localizer.Text("overlay.menu.export_current", "Export current").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, MenuHide, localizer.Text("overlay.menu.hide", "Hide").c_str());
    AppendMenuW(menu, MF_STRING, MenuExit, localizer.Text("overlay.menu.exit", "Exit").c_str());
    const UINT command = TrackPopupMenu(menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        screen.x, screen.y, 0, g_app.window, nullptr);
    if (command != 0) PostMessageW(g_app.window, WM_COMMAND, command, 0);
    DestroyMenu(menu);
}

void HandleMenu(const UINT id) {
    switch (id) {
    case MenuToggleLock: ToggleLock(); return;
    case MenuToggleCollapsed: ToggleCollapsed(); return;
    case MenuToggleTopmost: g_app.settings.topmost = !g_app.settings.topmost; ApplyWindowMode(); break;
    case MenuToggleHideIdle: g_app.settings.hide_idle = !g_app.settings.hide_idle; break;
    case MenuOpacity60: g_app.settings.opacity = 0.60F; break;
    case MenuOpacity75: g_app.settings.opacity = 0.75F; break;
    case MenuOpacity90: g_app.settings.opacity = 0.90F; break;
    case MenuScale100:
    case MenuScale125:
    case MenuScale150: {
        const float next_scale = id == MenuScale100 ? 1.0F :
            (id == MenuScale125 ? 1.25F : 1.5F);
        RECT rect{};
        GetWindowRect(g_app.window, &rect);
        const float dpi_scale =
            static_cast<float>(GetDpiForWindow(g_app.window)) / 96.0F;
        if (!g_app.settings.collapsed) {
            const auto logical = UnscaleWindowExtent(
                rect.right - rect.left, rect.bottom - rect.top,
                dpi_scale, g_app.settings.ui_scale);
            g_app.settings.width = logical.width;
            g_app.settings.height = logical.height;
        }
        g_app.settings.ui_scale = next_scale;
        const int collapsed_height = static_cast<int>(std::lround(
            nte::combat_stats::overlay::OverlayReferenceMetrics().title_height));
        const auto extent = ScaleWindowExtent(
            g_app.settings.width,
            g_app.settings.collapsed ? collapsed_height : g_app.settings.height,
            dpi_scale, next_scale);
        SetWindowPos(g_app.window, nullptr, 0, 0,
            extent.width, extent.height,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        break;
    }
    case MenuExportCurrent: BeginExport(); return;
    case MenuShow: ShowWindow(g_app.window, SW_SHOWNOACTIVATE); return;
    case MenuHide: ShowWindow(g_app.window, SW_HIDE); return;
    case MenuExit: DestroyWindow(g_app.window); return;
    default: return;
    }
    SaveSettings();
    RenderOverlay();
}

void HandleUiAction(const HitTarget& target) {
    switch (target.action) {
    case UiAction::ManualEnd:
        if (g_app.manual_end_event != nullptr && SetEvent(g_app.manual_end_event)) {
            g_app.action_status = Narrow(localizer.Text("overlay.end.running", "Ending the current encounter..."));
        } else {
            g_app.action_status = Narrow(localizer.Text("overlay.end.err_channel", "Manual end failed: the plugin control channel is unavailable"));
        }
        break;
    case UiAction::ToggleHistory:
        g_app.page = g_app.page == ViewPage::Live
            ? ViewPage::HistoryList : ViewPage::Live;
        g_app.drill = DrillLevel::Players;
        g_app.scroll_offset = 0;
        g_app.navigation.page = static_cast<
            nte::combat_stats::overlay::ViewPage>(g_app.page);
        g_app.navigation.drill =
            nte::combat_stats::overlay::DrillLevel::Players;
        g_app.navigation.player_key.clear();
        g_app.navigation.source_key.clear();
        g_app.navigation.scroll_offset = 0;
        break;
    case UiAction::ToggleCollapsed:
        ToggleCollapsed();
        return;
    case UiAction::OpenMenu: {
        POINT point{};
        GetCursorPos(&point);
        ShowContextMenu(point);
        return;
    }
    case UiAction::Back:
        if (g_app.drill == DrillLevel::Events) g_app.drill = DrillLevel::Sources;
        else if (g_app.drill == DrillLevel::Sources) g_app.drill = DrillLevel::Players;
        else if (g_app.page == ViewPage::HistoryDetail) g_app.page = ViewPage::HistoryList;
        g_app.scroll_offset = 0;
        g_app.navigation.page = static_cast<
            nte::combat_stats::overlay::ViewPage>(g_app.page);
        g_app.navigation.drill = static_cast<
            nte::combat_stats::overlay::DrillLevel>(g_app.drill);
        if (g_app.drill == DrillLevel::Players) {
            g_app.navigation.player_key.clear();
            g_app.navigation.source_key.clear();
        } else if (g_app.drill == DrillLevel::Sources) {
            g_app.navigation.source_key.clear();
        }
        g_app.navigation.scroll_offset = 0;
        break;
    case UiAction::OutgoingTab:
        g_app.settings.active_tab = DataTab::Outgoing;
        g_app.drill = DrillLevel::Players;
        g_app.scroll_offset = 0;
        g_app.navigation.drill =
            nte::combat_stats::overlay::DrillLevel::Players;
        g_app.navigation.player_key.clear();
        g_app.navigation.source_key.clear();
        SaveSettings();
        break;
    case UiAction::IncomingTab:
        g_app.settings.active_tab = DataTab::Incoming;
        g_app.drill = DrillLevel::Players;
        g_app.scroll_offset = 0;
        g_app.navigation.drill = nte::combat_stats::overlay::DrillLevel::Players;
        g_app.navigation.player_key.clear();
        g_app.navigation.source_key.clear();
        SaveSettings();
        break;
    case UiAction::HealingDoneTab:
        g_app.settings.active_tab = DataTab::HealingDone;
        g_app.drill = DrillLevel::Players;
        g_app.scroll_offset = 0;
        g_app.navigation.drill = nte::combat_stats::overlay::DrillLevel::Players;
        g_app.navigation.player_key.clear();
        g_app.navigation.source_key.clear();
        SaveSettings();
        break;
    case UiAction::HealingReceivedTab:
        g_app.settings.active_tab = DataTab::HealingReceived;
        g_app.drill = DrillLevel::Players;
        g_app.scroll_offset = 0;
        g_app.navigation.drill =
            nte::combat_stats::overlay::DrillLevel::Players;
        g_app.navigation.player_key.clear();
        g_app.navigation.source_key.clear();
        SaveSettings();
        break;
    case UiAction::SelectRow:
        if (g_app.page == ViewPage::HistoryList &&
            target.index < g_app.displayed_history.size()) {
            const auto& row = g_app.displayed_history[target.index];
            if (row.kind ==
                nte::combat_stats::overlay::HistoryListRow::Kind::Account) {
                const auto key = nte::account::DirectoryName(row.account);
                if (!g_app.expanded_history_accounts.erase(key)) {
                    g_app.expanded_history_accounts.insert(key);
                }
                g_app.scroll_offset = 0;
                break;
            }
            if (!row.encounter) break;
            const auto& encounter = *row.encounter;
            {
                std::scoped_lock lock(g_app.snapshot_mutex);
                g_app.requested_account = row.account;
                g_app.requested_report_id = encounter.report_id;
                g_app.requested_encounter_id = encounter.encounter_id;
                if (g_app.history_detail &&
                    (g_app.history_detail->summary.account != row.account ||
                     g_app.history_detail->summary.report_id != encounter.report_id ||
                     g_app.history_detail->summary.encounter_id != encounter.encounter_id)) {
                    g_app.history_detail.reset();
                }
            }
            g_app.page = ViewPage::HistoryDetail;
            g_app.drill = DrillLevel::Players;
            g_app.navigation.page =
                nte::combat_stats::overlay::ViewPage::HistoryDetail;
            g_app.navigation.drill =
                nte::combat_stats::overlay::DrillLevel::Players;
        } else if (g_app.drill == DrillLevel::Players &&
                   target.index < g_app.displayed_players.size()) {
            const auto& player = g_app.displayed_players[target.index];
            g_app.selected_player = player.handle;
            g_app.navigation.player_key = PlayerKey(player);
            g_app.navigation.source_key.clear();
            g_app.drill = DrillLevel::Sources;
        } else if (g_app.drill == DrillLevel::Sources &&
                   target.index < g_app.displayed_sources.size()) {
            g_app.selected_source = g_app.displayed_sources[target.index];
            g_app.navigation.source_key = SourceKey(g_app.selected_source);
            g_app.drill = DrillLevel::Events;
        }
        g_app.scroll_offset = 0;
        g_app.navigation.drill = static_cast<
            nte::combat_stats::overlay::DrillLevel>(g_app.drill);
        g_app.navigation.scroll_offset = 0;
        break;
    }
    g_app.hovered_target.reset();
    RenderOverlay();
}

void ReloadSettingsFromDisk() {
    const auto previous = g_app.settings;
    int current_x = previous.x;
    int current_y = previous.y;
    int current_width = previous.width;
    int current_height = previous.height;
    RECT current_rect{};
    if (GetWindowRect(g_app.window, &current_rect)) {
        current_x = current_rect.left;
        current_y = current_rect.top;
        if (!previous.collapsed) {
            const float current_dpi =
                static_cast<float>(GetDpiForWindow(g_app.window)) / 96.0F;
            const auto logical = UnscaleWindowExtent(
                current_rect.right - current_rect.left,
                current_rect.bottom - current_rect.top,
                current_dpi, previous.ui_scale);
            current_width = logical.width;
            current_height = logical.height;
        }
    }
    if (!LoadSettings()) return;
    g_app.settings.x = current_x;
    g_app.settings.y = current_y;
    g_app.settings.width = current_width;
    g_app.settings.height = current_height;
    const float dpi = static_cast<float>(GetDpiForWindow(g_app.window)) / 96.0F;
    const int collapsed_height = static_cast<int>(std::lround(
        nte::combat_stats::overlay::OverlayReferenceMetrics().title_height));
    const auto extent = ScaleWindowExtent(
        g_app.settings.width,
        g_app.settings.collapsed ? collapsed_height : g_app.settings.height,
        dpi, g_app.settings.ui_scale);
    if (previous.collapsed != g_app.settings.collapsed ||
        previous.ui_scale != g_app.settings.ui_scale) {
        SetWindowPos(g_app.window, nullptr, 0, 0, extent.width, extent.height,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ApplyWindowMode();
    if (previous.hide_idle != g_app.settings.hide_idle) {
        std::optional<EncounterState> state;
        {
            std::scoped_lock lock(g_app.snapshot_mutex);
            if (g_app.snapshot) state = g_app.snapshot->combat.state;
        }
        if (nte::combat_stats::overlay::ShouldHideOverlay(
                g_app.settings.hide_idle, state)) {
            ShowWindow(g_app.window, SW_HIDE);
        }
        else if (!g_app.settings.hide_idle) {
            ShowWindow(g_app.window, SW_SHOWNOACTIVATE);
        }
    }
    SaveSettings();
    RenderOverlay();
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == g_app.taskbar_created) { AddTrayIcon(); return 0; }
    if (message == g_reload_settings_message) { ReloadSettingsFromDisk(); return 0; }
    if (message == g_close_overlay_message) { DestroyWindow(window); return 0; }
    if (message == kRestartMessage) { DestroyWindow(window); return 0; }
    switch (message) {
    case WM_NCCALCSIZE:
        if (wparam != 0) return 0;
        break;
    case WM_CREATE:
        AddTrayIcon();
        SetTimer(window, kStaleTimerId, 1000, nullptr);
        return 0;
    case kExportCompleteMessage: {
        {
            std::scoped_lock lock(g_app.snapshot_mutex);
            g_app.action_status = std::move(g_app.export_result_status);
            g_app.export_result_status.clear();
        }
        RenderOverlay();
        return 0;
    }
    case kSnapshotMessage: {
        bool hide{};
        bool active{};
        bool has_snapshot{};
        std::string control_status;
        {
            std::scoped_lock lock(g_app.snapshot_mutex);
            hide = nte::combat_stats::overlay::ShouldHideOverlay(
                g_app.settings.hide_idle,
                g_app.snapshot ? std::optional{g_app.snapshot->combat.state} :
                    std::nullopt);
            active = g_app.snapshot &&
                g_app.snapshot->combat.state == EncounterState::Active;
            has_snapshot = g_app.snapshot.has_value();
            if (g_app.snapshot) control_status = g_app.snapshot->control_status;
        }
        if (control_status == "manual_end_completed") {
            g_app.action_status = Narrow(localizer.Text("overlay.end.done", "The encounter was ended and written to history"));
        } else if (control_status == "manual_end_failed") {
            g_app.action_status = Narrow(localizer.Text("overlay.end.err_log", "Manual end failed: the combat log was not written completely"));
        } else if (has_snapshot &&
                   g_app.export_in_progress == false &&


                   (!active || !g_app.action_status.empty())) {
            g_app.action_status.clear();
        }
        if (g_app.page == ViewPage::Live) {
            std::string encounter_key;
            std::vector<std::string> player_keys;
            {
                std::scoped_lock lock(g_app.snapshot_mutex);
                if (g_app.snapshot) {
                    encounter_key = g_app.snapshot->report_id + "/" +
                        g_app.snapshot->encounter_id;
                    player_keys.reserve(g_app.snapshot->combat.players.size());
                    for (const auto& player : g_app.snapshot->combat.players) {
                        player_keys.push_back(player.identity_key.empty()
                            ? player.label : player.identity_key);
                    }
                }
            }
            g_app.navigation.page =
                nte::combat_stats::overlay::ViewPage::Live;
            g_app.navigation.drill = static_cast<
                nte::combat_stats::overlay::DrillLevel>(g_app.drill);
            g_app.navigation.scroll_offset = g_app.scroll_offset;
            nte::combat_stats::overlay::ReconcileLiveNavigation(
                g_app.navigation, encounter_key, player_keys);
            g_app.drill = static_cast<DrillLevel>(g_app.navigation.drill);
            g_app.scroll_offset = g_app.navigation.scroll_offset;
            if (g_app.drill == DrillLevel::Players) {
                g_app.selected_player = {};
                g_app.selected_source = {};
            }
        }
        ShowWindow(window, hide ? SW_HIDE : SW_SHOWNOACTIVATE);
        RenderOverlay();
        return 0;
    }
    case kHistoryRefreshMessage: {
        bool selected_history_missing = wparam != 0;
        if (g_app.page == ViewPage::HistoryDetail) {
            std::scoped_lock lock(g_app.snapshot_mutex);
            selected_history_missing = selected_history_missing ||
                (g_app.history_error.empty() &&
                !g_app.requested_encounter_id.empty() &&
                std::ranges::none_of(g_app.history, [&](const auto& group) {
                    return group.account == g_app.requested_account &&
                        std::ranges::any_of(group.encounters, [&](const auto& encounter) {
                            return encounter.report_id == g_app.requested_report_id &&
                                encounter.encounter_id == g_app.requested_encounter_id;
                        });
                }));
        }
        if (selected_history_missing) {
            g_app.page = ViewPage::HistoryList;
            g_app.drill = DrillLevel::Players;
            g_app.navigation.page =
                nte::combat_stats::overlay::ViewPage::HistoryList;
            g_app.navigation.drill =
                nte::combat_stats::overlay::DrillLevel::Players;
            g_app.navigation.player_key.clear();
            g_app.navigation.source_key.clear();
            g_app.scroll_offset = 0;
        }
        RenderOverlay();
        return 0;
    }
    case kShowMessage:
        ShowWindow(window, SW_SHOWNOACTIVATE);
        return 0;
    case WM_TIMER: {
        const auto now = std::chrono::steady_clock::now();
        bool exit{};
        {
            std::scoped_lock lock(g_app.snapshot_mutex);
            if (!g_app.lifetime_guarded.load() &&
                g_app.last_snapshot != std::chrono::steady_clock::time_point{} &&
                now - g_app.last_snapshot >= 30s) exit = true;
        }
        if (exit) DestroyWindow(window);
        return 0;
    }
    case WM_HOTKEY:
        if (wparam == kHotkeyId) ToggleLock();
        return 0;
    case WM_NCHITTEST: {
        if (g_app.settings.locked) return HTTRANSPARENT;
        RECT rect{};
        GetWindowRect(window, &rect);
        const float x = static_cast<float>(GET_X_LPARAM(lparam) - rect.left);
        const float y = static_cast<float>(GET_Y_LPARAM(lparam) - rect.top);
        const float scale = static_cast<float>(GetDpiForWindow(window)) / 96.0F *
            g_app.settings.ui_scale;
        std::size_t players{};
        { std::scoped_lock lock(g_app.snapshot_mutex); if (g_app.snapshot) players = g_app.snapshot->combat.players.size(); }
        const auto layout = CalculateLayout(static_cast<float>(rect.right - rect.left),
            static_cast<float>(rect.bottom - rect.top), scale,
            g_app.settings.collapsed, players);
        const HitRegion base_region = HitTest(layout, x, y, false);
        std::vector<nte::combat_stats::overlay::ViewRect> controls;
        controls.reserve(g_app.hit_targets.size());
        for (const auto& target : g_app.hit_targets) {
            controls.push_back({target.rect.left, target.rect.top,
                target.rect.right, target.rect.bottom});
        }
        const HitRegion region = nte::combat_stats::overlay::ResolveWindowHit(
            base_region, x, y, controls);
        switch (region) {
        case HitRegion::Title: return HTCAPTION;
        case HitRegion::ResizeLeft: return HTLEFT;
        case HitRegion::ResizeRight: return HTRIGHT;
        case HitRegion::ResizeTop: return HTTOP;
        case HitRegion::ResizeBottom: return HTBOTTOM;
        case HitRegion::ResizeTopLeft: return HTTOPLEFT;
        case HitRegion::ResizeTopRight: return HTTOPRIGHT;
        case HitRegion::ResizeBottomLeft: return HTBOTTOMLEFT;
        case HitRegion::ResizeGrip: return HTBOTTOMRIGHT;
        default: break;
        }
        return region == HitRegion::Title ? HTCAPTION : HTCLIENT;
    }
    case WM_SETCURSOR: {
        CURSORINFO info{sizeof(info)};
        if (!GetCursorInfo(&info) || (info.flags & CURSOR_SHOWING) == 0 ||
            GetCursor() == nullptr) return TRUE;
        LPCWSTR cursor = IDC_ARROW;
        switch (LOWORD(lparam)) {
        case HTLEFT: case HTRIGHT: cursor = IDC_SIZEWE; break;
        case HTTOP: case HTBOTTOM: cursor = IDC_SIZENS; break;
        case HTTOPLEFT: case HTBOTTOMRIGHT: cursor = IDC_SIZENWSE; break;
        case HTTOPRIGHT: case HTBOTTOMLEFT: cursor = IDC_SIZENESW; break;
        default: break;
        }
        SetCursor(LoadCursorW(nullptr, cursor));
        return TRUE;
    }
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        const float dpi = static_cast<float>(GetDpiForWindow(window)) / 96.0F;
        const int collapsed_height = static_cast<int>(std::lround(
            nte::combat_stats::overlay::OverlayReferenceMetrics().title_height));
        const auto minimum = ScaleWindowExtent(420,
            g_app.settings.collapsed ? collapsed_height : 180,
            dpi, g_app.settings.ui_scale);
        const auto maximum = ScaleWindowExtent(1400,
            g_app.settings.collapsed ? collapsed_height : 1000,
            dpi, g_app.settings.ui_scale);
        limits->ptMinTrackSize = {minimum.width, minimum.height};
        limits->ptMaxTrackSize = {maximum.width, maximum.height};
        return 0;
    }
    case WM_MOUSEMOVE: {
        const float x = static_cast<float>(GET_X_LPARAM(lparam));
        const float y = static_cast<float>(GET_Y_LPARAM(lparam));
        std::optional<std::size_t> hovered;
        for (std::size_t index{}; index < g_app.hit_targets.size(); ++index) {
            if (Contains(g_app.hit_targets[index].rect, x, y)) {
                hovered = index;
                break;
            }
        }
        if (hovered != g_app.hovered_target) {
            g_app.hovered_target = hovered;
            RenderOverlay();
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        if (delta > 0) {
            g_app.scroll_offset = g_app.scroll_offset > 2 ? g_app.scroll_offset - 3 : 0;
        } else if (delta < 0) {
            g_app.scroll_offset = (std::min)(g_app.maximum_scroll,
                g_app.scroll_offset + 3);
        }
        RenderOverlay();
        return 0;
    }
    case WM_LBUTTONUP: {
        const float click_x = static_cast<float>(GET_X_LPARAM(lparam));
        const float click_y = static_cast<float>(GET_Y_LPARAM(lparam));
        for (const auto& target : g_app.hit_targets) {
            if (Contains(target.rect, click_x, click_y)) {
                HandleUiAction(target);
                return 0;
            }
        }
        RECT rect{};
        GetClientRect(window, &rect);
        const float scale = static_cast<float>(GetDpiForWindow(window)) / 96.0F *
            g_app.settings.ui_scale;
        const float y = static_cast<float>(GET_Y_LPARAM(lparam));
        const auto header_layout = CalculateLayout(
            static_cast<float>(rect.right), static_cast<float>(rect.bottom), scale,
            false, 0);
        if (!g_app.settings.collapsed && g_app.page != ViewPage::HistoryList &&
            g_app.drill == DrillLevel::Players &&
            y >= header_layout.title_height &&
            y < header_layout.title_height + header_layout.header_height) {
            const float fraction = static_cast<float>(GET_X_LPARAM(lparam)) /
                static_cast<float>(std::max(1L, rect.right));
            if (g_app.settings.active_tab != DataTab::Outgoing) return 0;
            const auto& columns = nte::combat_stats::overlay::OutgoingSummaryColumns();
            float total_weight{};
            for (const auto& column : columns) total_weight += column.weight;
            std::size_t index{};
            float edge = columns[0].weight / total_weight;
            while (index + 1 < columns.size() && fraction > edge) {
                ++index;
                edge += columns[index].weight / total_weight;
            }
            if (g_app.settings.sort_column == columns[index].sort)
                g_app.settings.descending = !g_app.settings.descending;
            else { g_app.settings.sort_column = columns[index].sort; g_app.settings.descending = true; }
            SaveSettings();
            RenderOverlay();
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ClientToScreen(window, &point);
        ShowContextMenu(point);
        return 0;
    }
    case WM_COMMAND: HandleMenu(LOWORD(wparam)); return 0;
    case kTrayMessage:
        if (LOWORD(lparam) == WM_LBUTTONDBLCLK) ShowWindow(window, SW_SHOWNOACTIVATE);
        else if (LOWORD(lparam) == WM_RBUTTONUP) {
            POINT point{}; GetCursorPos(&point); ShowContextMenu(point);
        }
        return 0;
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        RenderOverlay();
        return 0;
    }
    case WM_SIZE: RenderOverlay(); return 0;
    case WM_DESTROY:
        SaveSettings();
        RemoveTrayIcon();
        UnregisterHotKey(window, kHotkeyId);
        KillTimer(window, kStaleTimerId);
        StopBackgroundWork();
        if (g_app.manual_end_event != nullptr) {
            CloseHandle(g_app.manual_end_event);
            g_app.manual_end_event = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool InitializeRendering() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            g_app.render.d2d.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(
                g_app.render.write.ReleaseAndGetAddressOf())))) return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&g_app.render.wic)))) return false;
    auto make_format = [](IDWriteFactory* factory, float size,
                          DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** result) {
        return factory->CreateTextFormat(L"Microsoft YaHei UI", nullptr, weight,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", result);
    };
    if (FAILED(make_format(g_app.render.write.Get(), 14.0F,
            DWRITE_FONT_WEIGHT_NORMAL, &g_app.render.regular))) return false;
    if (FAILED(make_format(g_app.render.write.Get(), 12.0F,
            DWRITE_FONT_WEIGHT_NORMAL, &g_app.render.small_format))) return false;
    if (FAILED(make_format(g_app.render.write.Get(), 14.0F,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, &g_app.render.bold))) return false;
    for (auto* format : {g_app.render.regular.Get(), g_app.render.small_format.Get(),
                         g_app.render.bold.Get()}) {
        if (!ConfigureTextFormat(format,
                nte::combat_stats::overlay::TextAlignment::Center)) return false;
    }
    return true;
}

std::uint16_t ParsePort() {
    int count{};
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    std::uint16_t port = 14514;
    if (arguments) {
        for (int i = 1; i + 1 < count; ++i) {
            if (wcscmp(arguments[i], L"--port") == 0) {
                wchar_t* end{};
                const unsigned long value = wcstoul(arguments[i + 1], &end, 10);
                if (end && *end == L'\0' && value >= 1 && value <= 65535)
                    port = static_cast<std::uint16_t>(value);
            }
        }
        LocalFree(arguments);
    }
    return port;
}

DWORD ParseHostProcessId() {
    int count{};
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    DWORD result{};
    if (arguments) {
        for (int index = 1; index + 1 < count; ++index) {
            if (wcscmp(arguments[index], L"--host-pid") != 0) continue;
            wchar_t* end{};
            const unsigned long value = wcstoul(arguments[index + 1], &end, 10);
            if (end != nullptr && *end == L'\0' && value != 0) {
                result = static_cast<DWORD>(value);
            }
        }
        LocalFree(arguments);
    }
    return result;
}

bool RestartRequested() {
    int count{};
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    bool restart{};
    if (arguments) {
        for (int i = 1; i < count; ++i) {
            if (wcscmp(arguments[i], L"--restart") == 0) {
                restart = true;
                break;
            }
        }
        LocalFree(arguments);
    }
    return restart;
}

bool TerminateMatchingOverlay(const HWND window) {
    if (!window) return false;
    DWORD process_id{};
    GetWindowThreadProcessId(window, &process_id);
    if (process_id == 0) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION |
        PROCESS_TERMINATE | SYNCHRONIZE, FALSE, process_id);
    if (!process) return false;
    std::array<wchar_t, 32768> target_path{};
    std::array<wchar_t, 32768> current_path{};
    DWORD target_size = static_cast<DWORD>(target_path.size());
    const bool same_executable = QueryFullProcessImageNameW(
            process, 0, target_path.data(), &target_size) != FALSE &&
        GetModuleFileNameW(nullptr, current_path.data(),
            static_cast<DWORD>(current_path.size())) != 0 &&
        _wcsicmp(target_path.data(), current_path.data()) == 0;
    const bool terminated = same_executable &&
        TerminateProcess(process, ERROR_PROCESS_ABORTED) != FALSE;
    if (terminated) static_cast<void>(WaitForSingleObject(process, 2000));
    CloseHandle(process);
    return terminated;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const bool restart = RestartRequested();
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (!mutex) return 1;
    const bool already_running = GetLastError() == ERROR_ALREADY_EXISTS;
    if (already_running) {
        const HWND existing = FindWindowW(kWindowClassName, kWindowTitle);
        if (existing) {
            PostMessageW(existing, restart ? kRestartMessage : kShowMessage, 0, 0);
        }
        DWORD wait = restart ? WaitForSingleObject(mutex, 5000) : WAIT_TIMEOUT;
        if (restart && wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED &&
            TerminateMatchingOverlay(existing)) {
            wait = WaitForSingleObject(mutex, 2000);
        }
        if (!restart || (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)) {
            CloseHandle(mutex);
            return 0;
        }
    } else if (WaitForSingleObject(mutex, 0) != WAIT_OBJECT_0) {
        CloseHandle(mutex);
        return 1;
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) { CloseHandle(mutex); return 1; }
    g_app.instance = instance;
    std::array<wchar_t, 32768> executable_path{};
    const DWORD executable_length = GetModuleFileNameW(
        nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size()));
    if (executable_length != 0 && executable_length < executable_path.size()) {
        const auto executable_root =
            std::filesystem::path(executable_path.data()).parent_path();
        g_app.executable_root = executable_root;
        g_app.attribute_colors = nte::combat_stats::AttributeColorMap::Load(
            executable_root / "assets/attribute_colors.json",
            g_app.attribute_color_error);
    }
    g_app.port = ParsePort();
    const DWORD host_process_id = ParseHostProcessId();
    g_app.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    g_reload_settings_message = RegisterWindowMessageW(
        nte::combat_stats::overlay::kReloadSettingsMessageName);
    g_close_overlay_message = RegisterWindowMessageW(
        nte::combat_stats::overlay::kCloseOverlayMessageName);
    static_cast<void>(LoadSettings());
    if (!InitializeRendering()) { CoUninitialize(); CloseHandle(mutex); return 2; }

    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = WindowProcedure;
    window_class.hInstance = instance;
    window_class.hCursor = nullptr;
    window_class.lpszClassName = kWindowClassName;
    if (!RegisterClassExW(&window_class)) { CoUninitialize(); CloseHandle(mutex); return 3; }
    const int collapsed_height = static_cast<int>(std::lround(
        nte::combat_stats::overlay::OverlayReferenceMetrics().title_height));
    const auto initial_extent = ScaleWindowExtent(
        g_app.settings.width,
        g_app.settings.collapsed ? collapsed_height : g_app.settings.height,
        static_cast<float>(GetDpiForSystem()) / 96.0F,
        g_app.settings.ui_scale);
    g_app.window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWindowClassName, kWindowTitle, WS_POPUP | WS_THICKFRAME,
        g_app.settings.x, g_app.settings.y, initial_extent.width, initial_extent.height,
        nullptr, nullptr, instance, nullptr);
    if (!g_app.window) { CoUninitialize(); CloseHandle(mutex); return 4; }
    const auto monitor_extent = ScaleWindowExtent(
        g_app.settings.width,
        g_app.settings.collapsed ? collapsed_height : g_app.settings.height,
        static_cast<float>(GetDpiForWindow(g_app.window)) / 96.0F,
        g_app.settings.ui_scale);
    SetWindowPos(g_app.window, nullptr, 0, 0,
        monitor_extent.width, monitor_extent.height,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    ApplyWindowMode();
    RegisterHotKey(g_app.window, kHotkeyId, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F12);
    ShowWindow(g_app.window, SW_SHOWNOACTIVATE);
    RenderOverlay();
    g_app.last_snapshot = std::chrono::steady_clock::now();
    g_app.receiver = std::jthread(ReceiveSnapshots);
    g_app.lifetime_watcher = std::jthread(WatchLifetime, host_process_id);
    g_app.manual_end_event = OpenEventW(EVENT_MODIFY_STATE, FALSE,
        nte::combat_stats::overlay::kManualEndEventName);
    g_app.history_loader = std::jthread(LoadHistoryData);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    g_app.render = {};
    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return static_cast<int>(message.wParam);
}
