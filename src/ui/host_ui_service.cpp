#include "anomaly/host_ui_service.hpp"
#include "anomaly/thread_local_value.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace anomaly {
namespace {

std::atomic_bool g_menus_collapsed{};
std::atomic_bool g_menu_state_pending{true};
std::atomic_bool g_management_expansion_requested{};
std::atomic_bool g_developer_mode{};
std::atomic<PlatformInputCapturePolicy> g_input_capture_policy{
    PlatformInputCapturePolicy::Automatic};
ThreadLocalScalar<bool> g_apply_menu_state;
ThreadLocalScalar<bool> g_frame_menus_collapsed;
std::unordered_map<ImGuiID, bool> g_window_locks;

struct HostUiWindowScope {
    bool plugin_body_child_open{};
    ImGuiWindow* plugin_body_window{};
};

ThreadLocalObject<std::vector<HostUiWindowScope>> g_host_ui_window_scopes;

constexpr float kPluginWindowHeaderHeight = 42.0F;
constexpr float kPluginWindowBodyPadding = 12.0F;
constexpr float kPluginWindowActionSize = 30.0F;
constexpr float kPluginWindowActionGap = 4.0F;
constexpr float kPluginWindowActionRightInset = 12.0F;

struct PluginWindowChromeState {
    bool manually_collapsed{};
    bool host_collapsed{};
    bool pinned{};
    bool auto_fit_after_expand{};
    bool auto_fit_recovery{};
    std::uint8_t body_auto_fit_frames_x{};
    std::uint8_t body_auto_fit_frames_y{};
    ImVec2 expanded_size{};
};

std::unordered_map<ImGuiID, PluginWindowChromeState> g_plugin_window_chrome;

bool IsWindowLocked(const ImGuiID id) noexcept {
    const auto found = g_window_locks.find(id);
    return found != g_window_locks.end() && found->second;
}

bool IsPluginWindowCollapsed(const PluginWindowChromeState& state) noexcept {
    return state.manually_collapsed || state.host_collapsed;
}

bool HasPluginWindowExpandedSize(const PluginWindowChromeState& state) noexcept {
    return state.expanded_size.x > 0.0F ||
        state.expanded_size.y > kPluginWindowHeaderHeight;
}

bool PluginWindowHasPendingAutoFit(const ImGuiWindow& window) noexcept {
    return window.AutoFitFramesX > 0 || window.AutoFitFramesY > 0;
}

bool CapturePluginWindowExpandedSize(
    PluginWindowChromeState& state, const ImGuiWindow& window) noexcept {
    const bool auto_fit_x = window.AutoFitFramesX > 0;
    const bool auto_fit_y = window.AutoFitFramesY > 0;
    // A zero dimension tells ImGui to preserve auto-fit on that axis. Retain
    // fixed dimensions independently so a 360x0 request does not become 0x0.
    if (auto_fit_x) {
        state.expanded_size.x = 0.0F;
    } else if (window.SizeFull.x > 0.0F) {
        state.expanded_size.x = window.SizeFull.x;
    }
    if (auto_fit_y) {
        state.expanded_size.y = 0.0F;
    } else if (window.SizeFull.y > kPluginWindowHeaderHeight) {
        state.expanded_size.y = window.SizeFull.y;
    }
    return auto_fit_x || auto_fit_y;
}

bool PluginWindowNeedsAutoFitRecovery(const PluginWindowChromeState& state) noexcept {
    return state.auto_fit_after_expand || state.expanded_size.x <= 0.0F ||
        state.expanded_size.y <= 0.0F;
}

bool SynchronizePluginWindowHostCollapse(
    PluginWindowChromeState& state, const ImGuiWindow& window) noexcept {
    const bool was_collapsed = IsPluginWindowCollapsed(state);
    const bool host_was_collapsed = state.host_collapsed;
    state.host_collapsed =
        g_frame_menus_collapsed.Get() && !state.pinned && !IsWindowLocked(window.ID);
    const bool collapsed = IsPluginWindowCollapsed(state);
    if (!host_was_collapsed && state.host_collapsed && !state.manually_collapsed) {
        state.auto_fit_after_expand = CapturePluginWindowExpandedSize(state, window);
    }
    return was_collapsed != collapsed;
}

void SetNextPluginWindowCollapsedSize(const ImGuiWindow& window) {
    ImGui::SetNextWindowSize(
        ImVec2(window.SizeFull.x, kPluginWindowHeaderHeight), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(0.0F, kPluginWindowHeaderHeight),
        ImVec2((std::numeric_limits<float>::max)(), kPluginWindowHeaderHeight));
}

void SetPluginWindowSizeFromChromeState(
    ImGuiWindow& window, PluginWindowChromeState& state) {
    if (IsPluginWindowCollapsed(state)) {
        ImGui::SetWindowSize(
            &window, ImVec2(window.SizeFull.x, kPluginWindowHeaderHeight), ImGuiCond_Always);
    } else if (HasPluginWindowExpandedSize(state) || state.auto_fit_after_expand) {
        state.auto_fit_recovery = PluginWindowNeedsAutoFitRecovery(state);
        state.auto_fit_after_expand = false;
        ImGui::SetWindowSize(&window, state.expanded_size, ImGuiCond_Always);
    }
}

void SetNextPluginWindowSizeFromChromeState(
    const ImGuiWindow& window, PluginWindowChromeState& state) {
    if (IsPluginWindowCollapsed(state)) {
        SetNextPluginWindowCollapsedSize(window);
    } else if (HasPluginWindowExpandedSize(state) || state.auto_fit_after_expand) {
        state.auto_fit_recovery = PluginWindowNeedsAutoFitRecovery(state);
        state.auto_fit_after_expand = false;
        ImGui::SetNextWindowSize(state.expanded_size, ImGuiCond_Always);
    }
}

enum class PluginWindowChromeGlyph {
    ChevronDown,
    ChevronUp,
    Pin,
    Close,
};

ImVec2 Offset(const ImVec2 value, const float x, const float y) noexcept {
    return {value.x + x, value.y + y};
}

ImU32 ToU32(const ImVec4& color) noexcept {
    return ImGui::ColorConvertFloat4ToU32(color);
}

const char* PluginWindowChromeGlyphText(const PluginWindowChromeGlyph glyph) noexcept {
    switch (glyph) {
    case PluginWindowChromeGlyph::ChevronDown: return "\xEE\x9C\x8D";
    case PluginWindowChromeGlyph::ChevronUp: return "\xEE\x9C\x8E";
    case PluginWindowChromeGlyph::Pin: return "\xEE\x9C\x98";
    case PluginWindowChromeGlyph::Close: return "\xEE\x9C\x91";
    }
    return "";
}

bool IsManagementShellTitle(const std::string_view title) noexcept {
    return title.find("###management-shell") != std::string_view::npos ||
        title.find(";management-shell;") != std::string_view::npos;
}

std::string EllipsizeWindowTitle(const std::string_view value, const float available) {
    if (available <= 0.0F) return {};
    if (ImGui::CalcTextSize(value.data(), value.data() + value.size()).x <= available) {
        return std::string(value);
    }
    constexpr std::string_view suffix{"..."};
    if (ImGui::CalcTextSize(suffix.data(), suffix.data() + suffix.size()).x > available) return {};
    std::size_t end = value.size();
    while (end > 0U) {
        --end;
        while (end > 0U && (static_cast<unsigned char>(value[end]) & 0xc0U) == 0x80U) {
            --end;
        }
        std::string candidate{value.substr(0U, end)};
        candidate += suffix;
        if (ImGui::CalcTextSize(candidate.c_str()).x <= available) return candidate;
    }
    return std::string(suffix);
}

bool DrawPluginWindowChromeButton(
    ImGuiWindow& window, const ImRect& bounds, const char* const id_label,
    const PluginWindowChromeGlyph glyph, const char* const tooltip,
    const bool enabled, const bool selected = false) {
    const ImGuiID id = window.GetID(id_label);
    bool hovered{};
    bool held{};
    bool pressed{};
    if (enabled) {
        ImGui::KeepAliveID(id);
        pressed = ImGui::ButtonBehavior(bounds, id, &hovered, &held, ImGuiButtonFlags_NoNavFocus);
    } else {
        hovered = ImGui::IsMouseHoveringRect(bounds.Min, bounds.Max);
    }

    const ImVec4 raised(0.137F, 0.165F, 0.192F, 1.0F);
    const ImVec4 active(0.180F, 0.220F, 0.250F, 1.0F);
    const ImVec4 accent(0.345F, 0.718F, 0.647F, 1.0F);
    const ImVec4 transparent(0.0F, 0.0F, 0.0F, 0.0F);
    const ImVec4 background = held ? active : hovered ? raised
        : selected ? ImVec4(accent.x, accent.y, accent.z, 0.16F) : transparent;
    const ImVec4 text = !enabled ? ImVec4(0.451F, 0.490F, 0.533F, 1.0F)
        : selected ? accent : ImVec4(0.667F, 0.698F, 0.737F, 1.0F);
    ImDrawList* const draw_list = window.DrawList;
    draw_list->AddRectFilled(bounds.Min, bounds.Max, ToU32(background), 3.0F);
    const char* const glyph_text = PluginWindowChromeGlyphText(glyph);
    // Plugin content can select a CJK font, but these are host-owned icon glyphs.
    ImFont* const glyph_font = ImGui::GetIO().FontDefault;
    const float glyph_font_size = glyph_font != nullptr ? glyph_font->FontSize : ImGui::GetFontSize();
    const ImVec2 glyph_size = glyph_font != nullptr
        ? glyph_font->CalcTextSizeA(glyph_font_size, FLT_MAX, 0.0F, glyph_text)
        : ImGui::CalcTextSize(glyph_text);
    draw_list->AddText(glyph_font, glyph_font_size,
        Offset(bounds.Min, (bounds.GetWidth() - glyph_size.x) * 0.5F,
            (bounds.GetHeight() - glyph_size.y) * 0.5F - 1.0F),
        ToU32(text), glyph_text);
    if (hovered && tooltip != nullptr) ImGui::SetTooltip("%s", tooltip);
    return pressed;
}

void DrawPluginWindowDragRegion(ImGuiWindow& window, const ImRect& bounds, const bool movable) {
    if (!movable || bounds.GetWidth() <= 0.0F) return;
    bool hovered{};
    bool held{};
    const ImGuiID id = window.GetID("#ANOMALY_PLUGIN_WINDOW_DRAG");
    ImGui::KeepAliveID(id);
    const bool pressed = ImGui::ButtonBehavior(
        bounds, id, &hovered, &held,
        ImGuiButtonFlags_NoNavFocus | ImGuiButtonFlags_PressedOnClick);
    if (pressed) ImGui::StartMouseMovingWindow(&window);
}

bool DrawPluginWindowChrome(
    ImGuiWindow& window, const std::string_view title, int* const open,
    const ImGuiWindowFlags flags, PluginWindowChromeState& state) {
    const ImVec2 origin = window.Pos;
    const ImVec2 size = window.Size;
    ImDrawList* const draw_list = window.DrawList;
    const ImRect header(origin, Offset(origin, size.x, kPluginWindowHeaderHeight));
    draw_list->AddRectFilled(header.Min, header.Max, ToU32(ImVec4(0.086F, 0.102F, 0.118F, 1.0F)));
    draw_list->AddLine(
        Offset(origin, 0.0F, kPluginWindowHeaderHeight - 1.0F),
        Offset(origin, size.x, kPluginWindowHeaderHeight - 1.0F),
        ToU32(ImVec4(0.204F, 0.235F, 0.271F, 1.0F)));

    const float close_x = size.x - kPluginWindowActionRightInset - kPluginWindowActionSize;
    const float pin_x = close_x - kPluginWindowActionGap - kPluginWindowActionSize;
    const float collapse_x = pin_x - kPluginWindowActionGap - kPluginWindowActionSize;
    const float title_width = (std::max)(0.0F, collapse_x - 20.0F);
    const std::size_t id_marker = title.find("###");
    const std::string visible_title = EllipsizeWindowTitle(
        id_marker == std::string_view::npos ? title : title.substr(0U, id_marker), title_width);
    draw_list->AddText(Offset(origin, kPluginWindowBodyPadding, 13.0F),
        ToU32(ImVec4(0.949F, 0.957F, 0.965F, 1.0F)), visible_title.c_str());

    ImGui::PushClipRect(header.Min, header.Max, true);
    DrawPluginWindowDragRegion(window,
        ImRect(origin, Offset(origin, (std::max)(0.0F, collapse_x - 4.0F), kPluginWindowHeaderHeight)),
        (flags & ImGuiWindowFlags_NoMove) == 0);
    // A host-wide collapse owns the window height until the host is expanded
    // again. Do not turn that temporary state into a persistent manual collapse.
    const bool host_collapse_owns_state = state.host_collapsed;
    const bool collapse_enabled =
        (flags & ImGuiWindowFlags_NoCollapse) == 0 && !host_collapse_owns_state;
    const char* const collapse_tooltip = (flags & ImGuiWindowFlags_NoCollapse) != 0
        ? "This window cannot be collapsed"
        : host_collapse_owns_state
        ? "Expand the host interface first"
        : IsPluginWindowCollapsed(state) ? "Expand window" : "Collapse window";
    if (DrawPluginWindowChromeButton(window,
            ImRect(Offset(origin, collapse_x, 6.0F),
                Offset(origin, collapse_x + kPluginWindowActionSize, 6.0F + kPluginWindowActionSize)),
            "#ANOMALY_PLUGIN_WINDOW_COLLAPSE",
            IsPluginWindowCollapsed(state) ? PluginWindowChromeGlyph::ChevronUp
                                           : PluginWindowChromeGlyph::ChevronDown,
            collapse_tooltip,
            collapse_enabled)) {
        const bool was_collapsed = IsPluginWindowCollapsed(state);
        if (state.manually_collapsed) {
            state.manually_collapsed = false;
        } else {
            if (!was_collapsed) {
                state.auto_fit_after_expand = CapturePluginWindowExpandedSize(state, window);
            }
            state.manually_collapsed = true;
        }
        if (was_collapsed != IsPluginWindowCollapsed(state)) {
            SetPluginWindowSizeFromChromeState(window, state);
        }
    }
    if (DrawPluginWindowChromeButton(window,
            ImRect(Offset(origin, pin_x, 6.0F),
                Offset(origin, pin_x + kPluginWindowActionSize, 6.0F + kPluginWindowActionSize)),
            "#ANOMALY_PLUGIN_WINDOW_PIN", PluginWindowChromeGlyph::Pin,
            state.pinned ? "Remove from top" : "Keep above other plugin windows", true, state.pinned)) {
        state.pinned = !state.pinned;
        if (SynchronizePluginWindowHostCollapse(state, window)) {
            SetPluginWindowSizeFromChromeState(window, state);
        }
        if (state.pinned) ImGui::FocusWindow(&window);
    }
    if (DrawPluginWindowChromeButton(window,
            ImRect(Offset(origin, close_x, 6.0F),
                Offset(origin, close_x + kPluginWindowActionSize, 6.0F + kPluginWindowActionSize)),
            "#ANOMALY_PLUGIN_WINDOW_CLOSE", PluginWindowChromeGlyph::Close,
            open != nullptr ? "Close window" : "This window cannot be closed", open != nullptr)) {
        *open = 0;
    }
    ImGui::PopClipRect();

    if (state.pinned) ImGui::BringWindowToDisplayFront(&window);
    return open == nullptr || *open != 0;
}

ImGuiWindow* CurrentHostWindow() noexcept {
    if (GImGui == nullptr) return nullptr;
    ImGuiWindow* const current = ImGui::GetCurrentWindow();
    if (current == nullptr) return nullptr;
    return current->RootWindow != nullptr ? current->RootWindow : current;
}

bool DrawWindowLockButton(ImGuiWindow& window, bool locked) {
    ImGuiContext& context = *GImGui;
    const ImGuiStyle& style = context.Style;
    const ImRect title_bar = window.TitleBarRect();
    const float size = context.FontSize;
    const ImVec2 minimum(
        title_bar.Min.x + style.FramePadding.x + size + style.ItemInnerSpacing.x,
        title_bar.Min.y + style.FramePadding.y);
    const ImRect bounds(minimum, ImVec2(minimum.x + size, minimum.y + size));
    bool hovered{};
    bool held{};
    const ImGuiID id = window.GetID("#HOST_WINDOW_LOCK");
    ImGui::PushClipRect(title_bar.Min, title_bar.Max, false);
    ImGui::KeepAliveID(id);
    const bool pressed = ImGui::ButtonBehavior(
        bounds, id, &hovered, &held,
        ImGuiButtonFlags_NoNavFocus | ImGuiButtonFlags_PressedOnClick);

    ImDrawList* draw_list = window.DrawList;
    if (hovered || held) {
        draw_list->AddRectFilled(
            bounds.Min, bounds.Max,
            ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered),
            style.FrameRounding);
    }
    const ImU32 color = ImGui::GetColorU32(
        locked ? ImGuiCol_CheckMark : ImGuiCol_TextDisabled);
    const float left = bounds.Min.x + size * 0.27F;
    const float right = bounds.Max.x - size * 0.27F;
    const float body_top = bounds.Min.y + size * 0.48F;
    const float bottom = bounds.Max.y - size * 0.18F;
    draw_list->AddRect(
        ImVec2(left, body_top), ImVec2(right, bottom), color, 1.0F, 0, 1.5F);
    const float shackle_right = locked ? right - size * 0.08F : right + size * 0.10F;
    draw_list->PathLineTo(ImVec2(left + size * 0.10F, body_top));
    draw_list->PathBezierCubicCurveTo(
        ImVec2(left + size * 0.10F, bounds.Min.y + size * 0.20F),
        ImVec2(shackle_right, bounds.Min.y + size * 0.20F),
        ImVec2(shackle_right, body_top));
    draw_list->PathStroke(color, 0, 1.5F);
    ImGui::PopClipRect();
    if (hovered) ImGui::SetTooltip(locked ? "Unlock this window" : "Lock this window open");
    return pressed;
}

std::string Copy(AnomalyStringViewV1 value) {
    return value.data == nullptr ? std::string{} : std::string(value.data, value.size);
}

void ANOMALY_CALL SetNextWindowSize(void*, float width, float height, std::uint32_t condition) {
    ImGui::SetNextWindowSize(ImVec2(width, height), static_cast<ImGuiCond>(condition));
}

int ANOMALY_CALL BeginWindow(
    void*, AnomalyStringViewV1 title, int* open, std::uint32_t flags) {
    const std::string id = Copy(title);
    std::string text = id;
    if (id.find("###") == std::string::npos) {
        text += "###";
        text += id;
    }
    const bool management_shell = IsManagementShellTitle(text);
    ImGuiWindowFlags window_flags = static_cast<ImGuiWindowFlags>(flags);
    ImGuiWindow* const existing = ImGui::FindWindowByName(text.c_str());
    if (!management_shell) {
        window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse;
        if (existing != nullptr) {
            PluginWindowChromeState& chrome = g_plugin_window_chrome[existing->ID];
            const bool collapse_changed = SynchronizePluginWindowHostCollapse(chrome, *existing);
            if (IsPluginWindowCollapsed(chrome) ||
                (collapse_changed &&
                    (HasPluginWindowExpandedSize(chrome) || chrome.auto_fit_after_expand))) {
                SetNextPluginWindowSizeFromChromeState(*existing, chrome);
            }
        }
    }
    if ((window_flags & ImGuiWindowFlags_NoTitleBar) != 0 && existing != nullptr &&
        existing->Collapsed) {
        ImGui::SetWindowCollapsed(existing, false, ImGuiCond_Always);
    }
    bool visible = open == nullptr || *open != 0;
    if (!management_shell) {
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowPadding, ImVec2(kPluginWindowBodyPadding, kPluginWindowBodyPadding));
    }
    const bool result = ImGui::Begin(text.c_str(), open == nullptr ? nullptr : &visible, window_flags);
    if (!management_shell) ImGui::PopStyleVar();
    g_host_ui_window_scopes.Get().emplace_back();
    if (open != nullptr) *open = visible ? 1 : 0;
    const bool window_available = UpdateHostUiWindowState();
    if (management_shell || !result || !window_available || (open != nullptr && *open == 0)) {
        return result && window_available && (open == nullptr || *open != 0) ? 1 : 0;
    }

    ImGuiWindow* const window = ImGui::GetCurrentWindow();
    if (window == nullptr) return 0;
    PluginWindowChromeState& chrome = g_plugin_window_chrome[window->ID];
    const bool collapse_changed = SynchronizePluginWindowHostCollapse(chrome, *window);
    if (collapse_changed) SetPluginWindowSizeFromChromeState(*window, chrome);
    const bool body_auto_fit_pending = chrome.body_auto_fit_frames_x > 0 ||
        chrome.body_auto_fit_frames_y > 0;
    const bool auto_fit_pending = PluginWindowHasPendingAutoFit(*window) || body_auto_fit_pending;
    if (!IsPluginWindowCollapsed(chrome) && !auto_fit_pending &&
        window->SizeFull.y > kPluginWindowHeaderHeight) {
        static_cast<void>(CapturePluginWindowExpandedSize(chrome, *window));
        chrome.auto_fit_recovery = false;
    }
    const bool open_after_header = DrawPluginWindowChrome(*window, text, open, window_flags, chrome);
    if (open != nullptr) *open = open_after_header ? 1 : 0;
    if (!open_after_header || IsPluginWindowCollapsed(chrome)) {
        chrome.body_auto_fit_frames_x = 0;
        chrome.body_auto_fit_frames_y = 0;
        return 0;
    }
    ImGui::SetCursorPos(ImVec2(kPluginWindowBodyPadding, kPluginWindowHeaderHeight));
    const bool root_auto_fit_x = window->AutoFitFramesX > 0;
    const bool root_auto_fit_y = window->AutoFitFramesY > 0;
    const bool root_always_auto_resize =
        (window->Flags & ImGuiWindowFlags_AlwaysAutoResize) != 0;
    if (root_auto_fit_x && chrome.body_auto_fit_frames_x == 0) {
        chrome.body_auto_fit_frames_x = 2;
    }
    if (root_auto_fit_y && chrome.body_auto_fit_frames_y == 0) {
        chrome.body_auto_fit_frames_y = 2;
    }
    const bool body_auto_fit_x = root_always_auto_resize || root_auto_fit_x ||
        chrome.body_auto_fit_frames_x > 0;
    const bool body_auto_fit_y = root_always_auto_resize || root_auto_fit_y ||
        chrome.body_auto_fit_frames_y > 0;
    ImGuiChildFlags body_flags = ImGuiChildFlags_AlwaysUseWindowPadding;
    if (body_auto_fit_x) body_flags |= ImGuiChildFlags_AutoResizeX;
    if (body_auto_fit_y) body_flags |= ImGuiChildFlags_AutoResizeY;
    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding, ImVec2(0.0F, kPluginWindowBodyPadding));
    const bool body_visible = ImGui::BeginChild(
        "##anomaly-plugin-window-body", ImVec2(0.0F, 0.0F),
        body_flags);
    ImGui::PopStyleVar();
    g_host_ui_window_scopes.Get().back().plugin_body_child_open = true;
    g_host_ui_window_scopes.Get().back().plugin_body_window = ImGui::GetCurrentWindow();
    if (chrome.body_auto_fit_frames_x > 0) {
        --chrome.body_auto_fit_frames_x;
        if (window->AutoFitFramesX == 0) window->AutoFitFramesX = 1;
    }
    if (chrome.body_auto_fit_frames_y > 0) {
        --chrome.body_auto_fit_frames_y;
        if (window->AutoFitFramesY == 0) window->AutoFitFramesY = 1;
    }
    return body_visible ? 1 : 0;
}

void ANOMALY_CALL EndWindow(void*) {
    bool plugin_body_child_open{};
    if (!g_host_ui_window_scopes.Get().empty()) {
        plugin_body_child_open =
            g_host_ui_window_scopes.Get().back().plugin_body_child_open;
        g_host_ui_window_scopes.Get().pop_back();
    }
    if (plugin_body_child_open) ImGui::EndChild();
    ImGui::End();
}

void ANOMALY_CALL Text(void*, AnomalyStringViewV1 value) {
    const std::string text = Copy(value);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

int ANOMALY_CALL Button(
    void*, AnomalyStringViewV1 label, float width, float height) {
    const std::string text = Copy(label);
    return ImGui::Button(text.c_str(), ImVec2(width, height)) ? 1 : 0;
}

int ANOMALY_CALL ButtonEnabled(
    void*, AnomalyStringViewV1 label, float width, float height, int enabled) {
    const std::string text = Copy(label);
    ImGui::BeginDisabled(enabled == 0);
    const bool pressed = ImGui::Button(text.c_str(), ImVec2(width, height));
    ImGui::EndDisabled();
    return enabled != 0 && pressed ? 1 : 0;
}

int ANOMALY_CALL Checkbox(void*, AnomalyStringViewV1 label, int* value) {
    if (value == nullptr) return 0;
    const std::string text = Copy(label);
    bool checked = *value != 0;
    const bool changed = ImGui::Checkbox(text.c_str(), &checked);
    *value = checked ? 1 : 0;
    return changed ? 1 : 0;
}

int ANOMALY_CALL SliderFloat(
    void*, AnomalyStringViewV1 label, float* value, float minimum, float maximum) {
    if (value == nullptr || !std::isfinite(minimum) || !std::isfinite(maximum) ||
        minimum >= maximum) return 0;
    const std::string text = Copy(label);
    return ImGui::SliderFloat(text.c_str(), value, minimum, maximum) ? 1 : 0;
}

int ANOMALY_CALL InputUInt32(
    void*, AnomalyStringViewV1 label, std::uint32_t* value,
    std::uint32_t step, std::uint32_t step_fast) {
    if (value == nullptr) return 0;
    const std::string text = Copy(label);
    ImU32 input = static_cast<ImU32>(*value);
    const ImU32 small_step = static_cast<ImU32>(step);
    const ImU32 fast_step = static_cast<ImU32>(step_fast);
    const bool changed = ImGui::InputScalar(
        text.c_str(), ImGuiDataType_U32, &input,
        step == 0U ? nullptr : &small_step,
        step_fast == 0U ? nullptr : &fast_step,
        "%u");
    if (changed) *value = static_cast<std::uint32_t>(input);
    return changed ? 1 : 0;
}

int ANOMALY_CALL InputDouble(
    void*, AnomalyStringViewV1 label, double* value, double step, double step_fast) {
    if (value == nullptr) return 0;
    const std::string text = Copy(label);
    return ImGui::InputDouble(text.c_str(), value, step, step_fast, "%.17g") ? 1 : 0;
}

int DigitInputFilter(ImGuiInputTextCallbackData* data) {
    if (data == nullptr || data->EventChar == 0) return 0;
    return data->EventChar >= static_cast<ImWchar>('0') &&
            data->EventChar <= static_cast<ImWchar>('9')
        ? 0 : 1;
}

int ANOMALY_CALL InputText(
    void*, AnomalyStringViewV1 label, char* buffer,
    std::size_t buffer_capacity, std::uint32_t flags) {
    constexpr std::uint32_t kKnownFlags = ANOMALY_UI_TEXT_INPUT_V1_DIGITS;
    if (buffer == nullptr || buffer_capacity == 0 ||
        (flags & ~kKnownFlags) != 0 ||
        std::find(buffer, buffer + buffer_capacity, '\0') == buffer + buffer_capacity) {
        return 0;
    }
    const std::string text = Copy(label);
    ImGuiInputTextFlags imgui_flags = ImGuiInputTextFlags_None;
    ImGuiInputTextCallback callback{};
    if ((flags & ANOMALY_UI_TEXT_INPUT_V1_DIGITS) != 0) {
        imgui_flags |= ImGuiInputTextFlags_CallbackCharFilter;
        callback = DigitInputFilter;
    }
    return ImGui::InputText(
        text.c_str(), buffer, buffer_capacity, imgui_flags, callback) ? 1 : 0;
}

int ANOMALY_CALL DeveloperModeEnabled(void*) {
    return g_developer_mode.load() ? 1 : 0;
}

int ANOMALY_CALL ColorEdit4(void*, AnomalyStringViewV1 label, float rgba[4]) {
    if (rgba == nullptr) return 0;
    const std::string text = Copy(label);
    return ImGui::ColorEdit4(text.c_str(), rgba) ? 1 : 0;
}

void ANOMALY_CALL Separator(void*) { ImGui::Separator(); }

int ANOMALY_CALL BeginChild(
    void*, AnomalyStringViewV1 id, float width, float height, std::uint32_t flags) {
    const std::string text = Copy(id);
    return ImGui::BeginChild(
        text.c_str(), ImVec2(width, height), static_cast<ImGuiChildFlags>(flags)) ? 1 : 0;
}

void ANOMALY_CALL EndChild(void*) { ImGui::EndChild(); }

int ANOMALY_CALL BeginTable(
    void*, AnomalyStringViewV1 id, std::int32_t columns, std::uint32_t flags,
    float outer_width, float outer_height) {
    if (columns <= 0) return 0;
    const std::string text = Copy(id);
    return ImGui::BeginTable(
        text.c_str(), columns, static_cast<ImGuiTableFlags>(flags),
        ImVec2(outer_width, outer_height)) ? 1 : 0;
}

void ANOMALY_CALL TableNextRow(void*) { ImGui::TableNextRow(); }

int ANOMALY_CALL TableNextColumn(void*) { return ImGui::TableNextColumn() ? 1 : 0; }

void ANOMALY_CALL EndTable(void*) { ImGui::EndTable(); }

int ANOMALY_CALL BeginMenu(void*, AnomalyStringViewV1 label, int enabled) {
    const std::string text = Copy(label);
    return ImGui::BeginMenu(text.c_str(), enabled != 0) ? 1 : 0;
}

void ANOMALY_CALL EndMenu(void*) { ImGui::EndMenu(); }

void ANOMALY_CALL OpenPopup(void*, AnomalyStringViewV1 id) {
    const std::string text = Copy(id);
    ImGui::OpenPopup(text.c_str());
}

int ANOMALY_CALL BeginPopupModal(
    void*, AnomalyStringViewV1 id, int* open, std::uint32_t flags) {
    const std::string text = Copy(id);
    bool visible = open == nullptr || *open != 0;
    const int result = ImGui::BeginPopupModal(
        text.c_str(), open == nullptr ? nullptr : &visible, static_cast<ImGuiWindowFlags>(flags)) ? 1 : 0;
    if (open != nullptr) *open = visible ? 1 : 0;
    return result;
}

void ANOMALY_CALL EndPopup(void*) { ImGui::EndPopup(); }

void ANOMALY_CALL CloseCurrentPopup(void*) { ImGui::CloseCurrentPopup(); }

int ANOMALY_CALL FilterMatch(
    void*, AnomalyStringViewV1 filter, AnomalyStringViewV1 value) {
    const std::string needle = Copy(filter);
    const std::string haystack = Copy(value);
    if (needle.empty()) return 1;
    const auto equal_folded = [](const char left, const char right) {
        return std::tolower(static_cast<unsigned char>(left)) ==
            std::tolower(static_cast<unsigned char>(right));
    };
    const auto found = std::search(
        haystack.begin(), haystack.end(), needle.begin(), needle.end(), equal_folded);
    return found != haystack.end() ? 1 : 0;
}

std::uint32_t ANOMALY_CALL FrameState(void*) {
    if (ImGui::GetCurrentContext() == nullptr) return ANOMALY_UI_FRAME_V1_NONE;
    const ImGuiIO& io = ImGui::GetIO();
    std::uint32_t state = ANOMALY_UI_FRAME_V1_NONE;
    if (ImGui::IsItemHovered()) state |= ANOMALY_UI_FRAME_V1_ITEM_HOVERED;
    if (ImGui::IsWindowFocused()) state |= ANOMALY_UI_FRAME_V1_WINDOW_FOCUSED;
    if (ImGui::IsItemActive()) state |= ANOMALY_UI_FRAME_V1_ITEM_ACTIVE;
    if (io.WantCaptureMouse) state |= ANOMALY_UI_FRAME_V1_WANT_CAPTURE_MOUSE;
    if (io.WantCaptureKeyboard) state |= ANOMALY_UI_FRAME_V1_WANT_CAPTURE_KEYBOARD;
    if (io.WantTextInput) state |= ANOMALY_UI_FRAME_V1_WANT_TEXT_INPUT;
    return state;
}

void ANOMALY_CALL SetNextWindowSizeConstraints(
    void*, const float minimum_width, const float minimum_height,
    const float maximum_width, const float maximum_height) {
    const ImVec2 minimum((std::max)(0.0F, minimum_width), (std::max)(0.0F, minimum_height));
    const ImVec2 maximum(
        maximum_width > 0.0F ? maximum_width : FLT_MAX,
        maximum_height > 0.0F ? maximum_height : FLT_MAX);
    ImGui::SetNextWindowSizeConstraints(minimum, maximum);
}

void ANOMALY_CALL GetWindowSize(void*, float* width, float* height) {
    if (width == nullptr || height == nullptr || ImGui::GetCurrentContext() == nullptr) return;
    ImVec2 size = ImGui::GetWindowSize();
    if (ImGuiWindow* const window = CurrentHostWindow()) {
        if (!g_host_ui_window_scopes.Get().empty() &&
            g_host_ui_window_scopes.Get().back().plugin_body_window ==
                ImGui::GetCurrentWindow()) {
            size = window->Size;
        }
        const auto found = IsManagementShellTitle(window->Name) ? g_plugin_window_chrome.end()
            : g_plugin_window_chrome.find(window->ID);
        if (found != g_plugin_window_chrome.end()) {
            PluginWindowChromeState& chrome = found->second;
            if (IsPluginWindowCollapsed(chrome)) {
                size = chrome.expanded_size;
            } else if (chrome.auto_fit_recovery) {
                if (PluginWindowHasPendingAutoFit(*window)) {
                    size = chrome.expanded_size;
                } else {
                    chrome.auto_fit_recovery = false;
                }
            }
        }
    }
    *width = size.x;
    *height = size.y;
}

bool Finite3(const double (&value)[3]) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

struct ProjectedEntityBox {
    std::array<ImVec2, 8> corners{};
    float minimum_x{};
    float minimum_y{};
    float maximum_x{};
    float maximum_y{};
};

struct ProjectionFrameCache {
    ImGuiContext* context{};
    int frame{-1};
    ImVec2 display{};
    AnomalyEspCameraV1 camera{};
    std::array<double, 3> forward{};
    std::array<double, 3> right{};
    std::array<double, 3> up{};
    double focal{};
    bool camera_valid{};
    AnomalyEspEntityBoundsV1 bounds{};
    ProjectedEntityBox projected{};
    bool bounds_cached{};
    bool bounds_visible{};
};

ThreadLocalObject<ProjectionFrameCache> g_projection_cache;

bool Same3(const double (&left)[3], const double (&right)[3]) noexcept {
    return left[0] == right[0] && left[1] == right[1] && left[2] == right[2];
}

bool SameCamera(
    const AnomalyEspCameraV1& left, const AnomalyEspCameraV1& right) noexcept {
    return Same3(left.position, right.position) && Same3(left.rotation, right.rotation) &&
        left.horizontal_fov_degrees == right.horizontal_fov_degrees;
}

bool SameBounds(
    const AnomalyEspEntityBoundsV1& left,
    const AnomalyEspEntityBoundsV1& right) noexcept {
    return Same3(left.center, right.center) && Same3(left.extent, right.extent);
}

ProjectionFrameCache& PrepareProjectionFrame(const AnomalyEspCameraV1& camera) {
    ProjectionFrameCache& cache = g_projection_cache.Get();
    ImGuiContext* context = ImGui::GetCurrentContext();
    const int frame = ImGui::GetFrameCount();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (cache.context == context && cache.frame == frame && cache.display.x == display.x &&
        cache.display.y == display.y && SameCamera(cache.camera, camera)) {
        return cache;
    }

    cache = {};
    cache.context = context;
    cache.frame = frame;
    cache.display = display;
    cache.camera = camera;
    if (!Finite3(camera.position) || !Finite3(camera.rotation) ||
        !std::isfinite(camera.horizontal_fov_degrees) ||
        camera.horizontal_fov_degrees <= 5.0F || camera.horizontal_fov_degrees >= 175.0F ||
        !std::isfinite(display.x) || !std::isfinite(display.y) ||
        display.x <= 1.0F || display.y <= 1.0F) {
        return cache;
    }

    constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;
    const double pitch = camera.rotation[0] * kDegreesToRadians;
    const double yaw = camera.rotation[1] * kDegreesToRadians;
    const double roll = camera.rotation[2] * kDegreesToRadians;
    const double cp = std::cos(pitch);
    const double sp = std::sin(pitch);
    const double cy = std::cos(yaw);
    const double sy = std::sin(yaw);
    const double cr = std::cos(roll);
    const double sr = std::sin(roll);
    cache.forward = {cp * cy, cp * sy, sp};
    cache.right = {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr};
    cache.up = {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp};
    cache.focal = static_cast<double>(display.x) * 0.5 /
        std::tan(static_cast<double>(camera.horizontal_fov_degrees) *
                 kDegreesToRadians * 0.5);
    cache.camera_valid = std::isfinite(cache.focal);
    return cache;
}

const ProjectedEntityBox* ProjectEntityBox(
    const AnomalyEspCameraV1* camera, const AnomalyEspEntityBoundsV1* bounds) {
    if (camera == nullptr || camera->struct_size < sizeof(*camera) ||
        bounds == nullptr || bounds->struct_size < sizeof(*bounds) ||
        ImGui::GetCurrentContext() == nullptr || !Finite3(bounds->center) ||
        !Finite3(bounds->extent) ||
        bounds->extent[0] <= 0.0 || bounds->extent[1] <= 0.0 || bounds->extent[2] <= 0.0) {
        return nullptr;
    }

    ProjectionFrameCache& cache = PrepareProjectionFrame(*camera);
    if (!cache.camera_valid) return nullptr;
    if (cache.bounds_cached && SameBounds(cache.bounds, *bounds)) {
        return cache.bounds_visible ? &cache.projected : nullptr;
    }

    cache.bounds = *bounds;
    cache.bounds_cached = true;
    cache.bounds_visible = false;
    ProjectedEntityBox& projected = cache.projected;
    projected.minimum_x = (std::numeric_limits<float>::max)();
    projected.minimum_y = (std::numeric_limits<float>::max)();
    projected.maximum_x = (std::numeric_limits<float>::lowest)();
    projected.maximum_y = (std::numeric_limits<float>::lowest)();

    const std::array<double, 3> center_delta{
        bounds->center[0] - camera->position[0],
        bounds->center[1] - camera->position[1],
        bounds->center[2] - camera->position[2]};
    const auto dot = [&center_delta](const std::array<double, 3>& axis) {
        return center_delta[0] * axis[0] + center_delta[1] * axis[1] +
            center_delta[2] * axis[2];
    };
    const double center_depth = dot(cache.forward);
    const double center_right = dot(cache.right);
    const double center_up = dot(cache.up);
    const double near_radius =
        std::abs(cache.forward[0]) * bounds->extent[0] +
        std::abs(cache.forward[1]) * bounds->extent[1] +
        std::abs(cache.forward[2]) * bounds->extent[2];
    if (!std::isfinite(center_depth) || center_depth - near_radius <= 1.0) return nullptr;

    for (unsigned corner = 0; corner < 8; ++corner) {
        const double x = (corner & 1u) == 0 ? -bounds->extent[0] : bounds->extent[0];
        const double y = (corner & 2u) == 0 ? -bounds->extent[1] : bounds->extent[1];
        const double z = (corner & 4u) == 0 ? -bounds->extent[2] : bounds->extent[2];
        const double depth = center_depth + x * cache.forward[0] +
            y * cache.forward[1] + z * cache.forward[2];
        const double horizontal = center_right + x * cache.right[0] +
            y * cache.right[1] + z * cache.right[2];
        const double vertical = center_up + x * cache.up[0] +
            y * cache.up[1] + z * cache.up[2];
        const double screen_x = static_cast<double>(cache.display.x) * 0.5 +
            horizontal * cache.focal / depth;
        const double screen_y = static_cast<double>(cache.display.y) * 0.5 -
            vertical * cache.focal / depth;
        if (!std::isfinite(screen_x) || !std::isfinite(screen_y)) return nullptr;
        const ImVec2 point(static_cast<float>(screen_x), static_cast<float>(screen_y));
        projected.corners[corner] = point;
        projected.minimum_x = (std::min)(projected.minimum_x, point.x);
        projected.minimum_y = (std::min)(projected.minimum_y, point.y);
        projected.maximum_x = (std::max)(projected.maximum_x, point.x);
        projected.maximum_y = (std::max)(projected.maximum_y, point.y);
    }
    if (projected.maximum_x - projected.minimum_x < 1.0F ||
        projected.maximum_y - projected.minimum_y < 1.0F || projected.maximum_x < 0.0F ||
        projected.maximum_y < 0.0F || projected.minimum_x > cache.display.x ||
        projected.minimum_y > cache.display.y) {
        return nullptr;
    }
    cache.bounds_visible = true;
    return &projected;
}

bool ValidStyle(const AnomalyEspBoxStyleV1* style) noexcept {
    return style == nullptr || style->struct_size >= sizeof(*style);
}

float LineThickness(const AnomalyEspBoxStyleV1* style) noexcept {
    return style != nullptr && std::isfinite(style->thickness) && style->thickness > 0.0F
        ? style->thickness : 1.5F;
}

float OutlineThickness(const AnomalyEspBoxStyleV1* style) noexcept {
    return style != nullptr && std::isfinite(style->outline_thickness) &&
            style->outline_thickness > 0.0F
        ? style->outline_thickness : 1.0F;
}

int ANOMALY_CALL DrawEntityBbox(
    void*, const AnomalyEspCameraV1* camera,
    const AnomalyEspEntityBoundsV1* bounds, const AnomalyEspBoxStyleV1* style) {
    if (!ValidStyle(style)) return 0;
    const ProjectedEntityBox* projected = ProjectEntityBox(camera, bounds);
    if (projected == nullptr) return 0;

    const std::uint32_t color = style == nullptr
        ? ANOMALY_RGBA_V1(80, 220, 120, 255) : style->color_rgba;
    const float thickness = LineThickness(style);
    const ImVec2 minimum(projected->minimum_x, projected->minimum_y);
    const ImVec2 maximum(projected->maximum_x, projected->maximum_y);
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    if (style != nullptr && (style->flags & ANOMALY_ESP_BOX_V1_OUTLINE) != 0) {
        const float outline = OutlineThickness(style);
        draw_list->AddRect(
            minimum, maximum, style->outline_color_rgba, 0.0F, 0, thickness + outline * 2.0F);
    }
    draw_list->AddRect(minimum, maximum, color, 0.0F, 0, thickness);
    return 1;
}

int ANOMALY_CALL DrawEntityBox3d(
    void*, const AnomalyEspCameraV1* camera,
    const AnomalyEspEntityBoundsV1* bounds, const AnomalyEspBoxStyleV1* style) {
    if (!ValidStyle(style)) return 0;
    const ProjectedEntityBox* projected = ProjectEntityBox(camera, bounds);
    if (projected == nullptr) return 0;

    constexpr std::array<std::array<unsigned, 2>, 12> edges{{
        {{0, 1}}, {{0, 2}}, {{0, 4}}, {{1, 3}}, {{1, 5}}, {{2, 3}},
        {{2, 6}}, {{3, 7}}, {{4, 5}}, {{4, 6}}, {{5, 7}}, {{6, 7}}}};
    const std::uint32_t color = style == nullptr
        ? ANOMALY_RGBA_V1(80, 220, 120, 255) : style->color_rgba;
    const float thickness = LineThickness(style);
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    if (style != nullptr && (style->flags & ANOMALY_ESP_BOX_V1_OUTLINE) != 0) {
        const float outline_thickness = thickness + OutlineThickness(style) * 2.0F;
        for (const auto& edge : edges) {
            draw_list->AddLine(
                projected->corners[edge[0]], projected->corners[edge[1]], style->outline_color_rgba,
                outline_thickness);
        }
    }
    for (const auto& edge : edges) {
        draw_list->AddLine(
            projected->corners[edge[0]], projected->corners[edge[1]], color, thickness);
    }
    return 1;
}

int ANOMALY_CALL DrawEntityLabel(
    void*, const AnomalyEspCameraV1* camera, const AnomalyEspEntityBoundsV1* bounds,
    AnomalyStringViewV1 value, std::uint32_t color) {
    if (value.data == nullptr || value.size == 0) return 0;
    const ProjectedEntityBox* projected = ProjectEntityBox(camera, bounds);
    if (projected == nullptr) return 0;
    const char* const text_end = value.data + value.size;
    const ImVec2 size = ImGui::CalcTextSize(value.data, text_end);
    const ImVec2 position(
        (projected->minimum_x + projected->maximum_x - size.x) * 0.5F,
        projected->minimum_y - size.y - 4.0F);
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    draw_list->AddRectFilled(
        ImVec2(position.x - 3.0F, position.y - 2.0F),
        ImVec2(position.x + size.x + 3.0F, position.y + size.y + 2.0F),
        ANOMALY_RGBA_V1(0, 0, 0, 180), 2.0F);
    draw_list->AddText(nullptr, 0.0F, position, color, value.data, text_end);
    return 1;
}

const AnomalyUiServiceV1 kUiService{
    sizeof(AnomalyUiServiceV1), ANOMALY_UI_SERVICE_V1_VERSION, nullptr,
    SetNextWindowSize, BeginWindow, EndWindow, Text, Button, DrawEntityBbox,
    Checkbox, SliderFloat, ColorEdit4, DrawEntityBox3d, DrawEntityLabel,
    Separator, BeginChild, EndChild, BeginTable, TableNextRow, TableNextColumn,
    EndTable, BeginMenu, EndMenu, OpenPopup, BeginPopupModal, EndPopup,
    CloseCurrentPopup, FilterMatch, FrameState,
    SetNextWindowSizeConstraints, GetWindowSize, InputUInt32, InputDouble,
    DeveloperModeEnabled, InputText, ButtonEnabled};

}  // namespace

const AnomalyUiServiceV1* HostUiServiceTable() noexcept {
    return &kUiService;
}

void SetHostUiMenusCollapsed(bool collapsed) noexcept {
    g_menus_collapsed.store(collapsed);
    g_menu_state_pending.store(true);
}

bool HostUiMenusCollapsed() noexcept { return g_menus_collapsed.load(); }

void RequestHostUiManagementExpansion() noexcept {
    g_management_expansion_requested.store(true);
    SetHostUiMenusCollapsed(false);
}

bool ConsumeHostUiManagementExpansionRequest() noexcept {
    return g_management_expansion_requested.exchange(false);
}

bool HostUiMenusCaptureMouse() noexcept {
    // While the platform menu is expanded the cursor must always be owned by
    // the overlay. The Automatic policy's WantCaptureMouse gate deadlocks here:
    // when the game hides the cursor it never lands on an ImGui window, so
    // capture stays false, NoMouse is set, ImGui ignores the mouse, and
    // WantCaptureMouse can never become true again.
    return !g_menus_collapsed.load();
}

void SetHostUiInputCapturePolicy(const PlatformInputCapturePolicy policy) noexcept {
    g_input_capture_policy.store(policy);
}

void SetHostUiDeveloperMode(const bool enabled) noexcept {
    g_developer_mode.store(enabled);
}

bool HostUiDeveloperModeEnabled() noexcept { return g_developer_mode.load(); }

bool HostUiCurrentWindowLocked() noexcept {
    ImGuiWindow* const window = CurrentHostWindow();
    if (window == nullptr) return false;
    const auto found = g_window_locks.find(window->ID);
    return found != g_window_locks.end() && found->second;
}

void SetHostUiCurrentWindowLocked(const bool locked) noexcept {
    ImGuiWindow* const window = CurrentHostWindow();
    if (window == nullptr) return;
    g_window_locks[window->ID] = locked;
}

void PrepareHostUiFrame() noexcept {
    g_apply_menu_state.Set(g_menu_state_pending.exchange(false));
    g_frame_menus_collapsed.Set(g_menus_collapsed.load());
}

bool UpdateHostUiWindowState() noexcept {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window == nullptr) return true;
    if (window->TitleBarHeight <= 0.0F) {
        if (window->Collapsed) ImGui::SetWindowCollapsed(window, false, ImGuiCond_Always);
        return true;
    }
    bool& locked = g_window_locks[window->ID];
    if (window->TitleBarHeight > 0.0F && DrawWindowLockButton(*window, locked)) locked = !locked;
    if (locked) {
        if (window->Collapsed) ImGui::SetWindowCollapsed(window, false, ImGuiCond_Always);
    } else if (g_frame_menus_collapsed.Get()) {
        if (!window->Collapsed) ImGui::SetWindowCollapsed(window, true, ImGuiCond_Always);
    } else if (g_apply_menu_state.Get() && window->Collapsed) {
        ImGui::SetWindowCollapsed(window, false, ImGuiCond_Always);
    }
    return !window->Collapsed;
}

}  // namespace anomaly
