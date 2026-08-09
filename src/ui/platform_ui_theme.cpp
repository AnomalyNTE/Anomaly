#include "anomaly/platform_ui_theme.hpp"

#include <imgui.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>
#include <atomic>

namespace ue5mem {
namespace {

constexpr PlatformUiColor Color(
    const float red, const float green, const float blue, const float alpha = 1.0f) {
    return {red, green, blue, alpha};
}

constexpr PlatformUiThemeColors kMoss{
    Color(0.949f, 0.957f, 0.965f), Color(0.451f, 0.490f, 0.533f),
    Color(0.078f, 0.090f, 0.102f), Color(0.106f, 0.125f, 0.145f),
    Color(0.137f, 0.165f, 0.192f), Color(0.204f, 0.235f, 0.271f),
    Color(0.106f, 0.125f, 0.145f), Color(0.137f, 0.165f, 0.192f),
    Color(0.150f, 0.180f, 0.208f), Color(0.106f, 0.125f, 0.145f),
    Color(0.137f, 0.165f, 0.192f), Color(0.180f, 0.260f, 0.255f),
    Color(0.345f, 0.718f, 0.647f), Color(0.415f, 0.773f, 0.706f),
    Color(0.290f, 0.635f, 0.570f), Color(0.345f, 0.718f, 0.647f, 0.16f),
    Color(0.380f, 0.788f, 0.545f), Color(0.882f, 0.675f, 0.322f),
    Color(0.898f, 0.435f, 0.447f), Color(0.431f, 0.659f, 0.996f),
    Color(0.063f, 0.129f, 0.122f), Color(0.063f, 0.075f, 0.090f),
    Color(0.086f, 0.102f, 0.118f), Color(0.090f, 0.106f, 0.122f),
    Color(0.118f, 0.141f, 0.165f), Color(0.125f, 0.149f, 0.173f),
    Color(0.094f, 0.114f, 0.133f), Color(0.298f, 0.337f, 0.380f),
    Color(0.851f, 0.973f, 0.945f), Color(0.125f, 0.149f, 0.173f),
    Color(0.306f, 0.349f, 0.396f), Color(0.118f, 0.141f, 0.165f, 0.98f),
};

constexpr PlatformUiThemeColors kAurora{
    Color(0.925f, 0.957f, 1.000f), Color(0.560f, 0.650f, 0.760f),
    Color(0.055f, 0.078f, 0.130f), Color(0.075f, 0.106f, 0.170f),
    Color(0.110f, 0.150f, 0.230f), Color(0.200f, 0.290f, 0.420f),
    Color(0.075f, 0.106f, 0.170f), Color(0.110f, 0.150f, 0.230f),
    Color(0.130f, 0.200f, 0.300f), Color(0.075f, 0.106f, 0.170f),
    Color(0.110f, 0.150f, 0.230f), Color(0.100f, 0.300f, 0.400f),
    Color(0.380f, 0.780f, 0.960f), Color(0.550f, 0.860f, 1.000f),
    Color(0.250f, 0.640f, 0.900f), Color(0.380f, 0.780f, 0.960f, 0.16f),
    Color(0.350f, 0.880f, 0.780f), Color(0.980f, 0.750f, 0.250f),
    Color(0.980f, 0.420f, 0.520f), Color(0.550f, 0.700f, 1.000f),
    Color(0.030f, 0.100f, 0.160f), Color(0.035f, 0.055f, 0.100f),
    Color(0.060f, 0.090f, 0.150f), Color(0.080f, 0.120f, 0.190f),
    Color(0.100f, 0.140f, 0.220f), Color(0.120f, 0.180f, 0.270f),
    Color(0.080f, 0.120f, 0.180f), Color(0.290f, 0.400f, 0.530f),
    Color(0.900f, 0.980f, 1.000f), Color(0.100f, 0.140f, 0.220f),
    Color(0.300f, 0.420f, 0.580f), Color(0.070f, 0.110f, 0.170f, 0.98f),
};

constexpr PlatformUiThemeColors kEmber{
    Color(1.000f, 0.950f, 0.910f), Color(0.710f, 0.630f, 0.580f),
    Color(0.090f, 0.065f, 0.050f), Color(0.140f, 0.090f, 0.070f),
    Color(0.210f, 0.130f, 0.100f), Color(0.320f, 0.220f, 0.170f),
    Color(0.140f, 0.090f, 0.070f), Color(0.210f, 0.130f, 0.100f),
    Color(0.250f, 0.150f, 0.110f), Color(0.140f, 0.090f, 0.070f),
    Color(0.210f, 0.130f, 0.100f), Color(0.360f, 0.190f, 0.130f),
    Color(0.940f, 0.480f, 0.320f), Color(1.000f, 0.620f, 0.450f),
    Color(0.780f, 0.300f, 0.200f), Color(0.940f, 0.480f, 0.320f, 0.16f),
    Color(0.400f, 0.820f, 0.630f), Color(0.960f, 0.730f, 0.350f),
    Color(1.000f, 0.400f, 0.380f), Color(0.550f, 0.700f, 0.930f),
    Color(0.170f, 0.060f, 0.030f), Color(0.065f, 0.045f, 0.035f),
    Color(0.120f, 0.075f, 0.060f), Color(0.160f, 0.100f, 0.080f),
    Color(0.190f, 0.120f, 0.095f), Color(0.230f, 0.140f, 0.105f),
    Color(0.150f, 0.090f, 0.070f), Color(0.430f, 0.300f, 0.240f),
    Color(0.990f, 0.910f, 0.860f), Color(0.170f, 0.110f, 0.085f),
    Color(0.450f, 0.300f, 0.230f), Color(0.150f, 0.095f, 0.075f, 0.98f),
};

constexpr PlatformUiThemeColors kPaper{
    Color(0.110f, 0.160f, 0.230f), Color(0.350f, 0.420f, 0.510f),
    Color(0.940f, 0.960f, 0.980f), Color(1.000f, 1.000f, 1.000f),
    Color(1.000f, 1.000f, 1.000f), Color(0.760f, 0.810f, 0.880f),
    Color(0.970f, 0.980f, 1.000f), Color(0.900f, 0.940f, 0.990f),
    Color(0.840f, 0.900f, 0.980f), Color(0.910f, 0.940f, 0.980f),
    Color(0.850f, 0.900f, 0.970f), Color(0.790f, 0.860f, 0.950f),
    Color(0.140f, 0.390f, 0.860f), Color(0.220f, 0.490f, 0.950f),
    Color(0.100f, 0.310f, 0.730f), Color(0.140f, 0.390f, 0.860f, 0.14f),
    Color(0.120f, 0.620f, 0.420f), Color(0.780f, 0.480f, 0.050f),
    Color(0.780f, 0.190f, 0.230f), Color(0.160f, 0.400f, 0.760f),
    Color(1.000f, 1.000f, 1.000f), Color(0.880f, 0.920f, 0.970f),
    Color(0.900f, 0.940f, 0.980f), Color(0.940f, 0.960f, 0.990f),
    Color(0.980f, 0.990f, 1.000f), Color(0.910f, 0.950f, 1.000f),
    Color(0.820f, 0.870f, 0.930f), Color(0.610f, 0.680f, 0.780f),
    Color(1.000f, 1.000f, 1.000f), Color(0.900f, 0.930f, 0.980f),
    Color(0.600f, 0.680f, 0.800f), Color(0.960f, 0.970f, 1.000f, 0.98f),
};

constexpr PlatformUiThemeColors kAnomalyHub{
    Color(0.950f, 0.950f, 0.950f), Color(0.700f, 0.700f, 0.700f),
    Color(0.106f, 0.106f, 0.106f), Color(0.161f, 0.161f, 0.161f),
    Color(0.161f, 0.161f, 0.161f), Color(0.350f, 0.350f, 0.350f),
    Color(0.161f, 0.161f, 0.161f), Color(0.161f, 0.161f, 0.161f),
    Color(0.161f, 0.161f, 0.161f), Color(0.161f, 0.161f, 0.161f),
    Color(0.161f, 0.161f, 0.161f), Color(0.161f, 0.161f, 0.161f),
    Color(1.000f, 0.639f, 0.102f), Color(1.000f, 0.725f, 0.302f),
    Color(0.820f, 0.420f, 0.020f), Color(1.000f, 0.639f, 0.102f, 0.16f),
    Color(0.360f, 0.820f, 0.560f), Color(1.000f, 0.639f, 0.102f),
    Color(0.940f, 0.320f, 0.300f), Color(0.420f, 0.660f, 0.960f),
    Color(0.106f, 0.106f, 0.106f), Color(0.106f, 0.106f, 0.106f),
    Color(0.161f, 0.161f, 0.161f), Color(0.161f, 0.161f, 0.161f),
    Color(0.161f, 0.161f, 0.161f), Color(0.161f, 0.161f, 0.161f),
    Color(0.106f, 0.106f, 0.106f), Color(0.350f, 0.350f, 0.350f),
    Color(1.000f, 0.930f, 0.800f), Color(0.161f, 0.161f, 0.161f),
    Color(0.350f, 0.350f, 0.350f), Color(0.161f, 0.161f, 0.161f, 0.98f),
};

constexpr PlatformUiColor MixColor(
    const PlatformUiColor& from, const PlatformUiColor& to, const float amount) noexcept {
    return Color(
        from.red + (to.red - from.red) * amount,
        from.green + (to.green - from.green) * amount,
        from.blue + (to.blue - from.blue) * amount,
        from.alpha + (to.alpha - from.alpha) * amount);
}

PlatformUiThemeColors BuildCustomTheme(const PlatformUiCustomColors& colors) noexcept {
    PlatformUiThemeColors theme = kAnomalyHub;
    theme.text = colors.text;
    theme.text_muted = MixColor(colors.child_background, colors.text, 0.62f);
    theme.window_background = colors.window_background;
    theme.child_background = colors.child_background;
    theme.popup_background = colors.child_background;
    theme.border = colors.border;
    theme.frame = colors.child_background;
    theme.frame_hovered = MixColor(colors.child_background, colors.accent, 0.12f);
    theme.frame_active = MixColor(colors.child_background, colors.accent, 0.22f);
    theme.button = colors.child_background;
    theme.button_hovered = MixColor(colors.child_background, colors.accent, 0.12f);
    theme.button_active = MixColor(colors.child_background, colors.accent, 0.22f);
    theme.accent = colors.accent;
    theme.accent_hovered = MixColor(colors.accent, Color(1.0f, 1.0f, 1.0f), 0.18f);
    theme.accent_active = MixColor(colors.accent, Color(0.0f, 0.0f, 0.0f), 0.18f);
    theme.accent_soft = Color(colors.accent.red, colors.accent.green, colors.accent.blue, 0.16f);
    const float accent_luminance = 0.2126f * colors.accent.red +
        0.7152f * colors.accent.green + 0.0722f * colors.accent.blue;
    theme.inverse_text = accent_luminance >= 0.58f
        ? Color(0.06f, 0.06f, 0.06f) : Color(0.96f, 0.96f, 0.96f);
    theme.navigation_background = colors.window_background;
    theme.header_background = colors.child_background;
    theme.toolbar_background = colors.child_background;
    theme.panel_background = colors.child_background;
    theme.row_hovered = MixColor(colors.child_background, colors.text, 0.08f);
    theme.toggle_off = colors.window_background;
    theme.toggle_off_border = colors.border;
    theme.toggle_on_knob = colors.text;
    theme.icon_fill = colors.child_background;
    theme.icon_border = colors.border;
    theme.toast_background = Color(colors.child_background.red,
        colors.child_background.green, colors.child_background.blue, 0.98f);
    return theme;
}

PlatformUiCustomColors g_platform_ui_custom_colors;
PlatformUiThemeColors g_platform_ui_custom_theme =
    BuildCustomTheme(g_platform_ui_custom_colors);
std::atomic<PlatformUiPalette> g_platform_ui_palette{PlatformUiPalette::AnomalyHub};

constexpr float kPlatformFontSizePixels = 13.0f;
constexpr std::array kPlatformFontBakeScales{1.0f, 1.25f, 1.70f};
constexpr std::string_view kPlatformFontNamePrefix{"Anomaly host "};

const std::vector<unsigned char>* CachedFontBytes(
    const std::filesystem::path& path) noexcept {
    try {
        static std::mutex mutex;
        static std::map<std::filesystem::path,
            std::unique_ptr<std::vector<unsigned char>>> cache;
        std::scoped_lock lock(mutex);
        if (const auto found = cache.find(path); found != cache.end()) {
            return found->second.get();
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) return nullptr;
        auto bytes = std::make_unique<std::vector<unsigned char>>(
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        if (bytes->empty() ||
            bytes->size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return nullptr;
        }
        const auto* result = bytes.get();
        cache.emplace(path, std::move(bytes));
        return result;
    } catch (...) {
        return nullptr;
    }
}

ImFont* AddCachedFontFromPath(
    ImFontAtlas& atlas,
    const std::filesystem::path& path,
    const float size_pixels,
    ImFontConfig configuration,
    const ImWchar* glyph_ranges) noexcept {
    try {
        const auto* bytes = CachedFontBytes(path);
        if (bytes == nullptr) return nullptr;
        configuration.FontDataOwnedByAtlas = false;
        return atlas.AddFontFromMemoryTTF(const_cast<unsigned char*>(bytes->data()),
            static_cast<int>(bytes->size()), size_pixels, &configuration, glyph_ranges);
    } catch (...) {
        return nullptr;
    }
}

void SetPlatformFontName(ImFontConfig& configuration, const float scale) noexcept {
    std::snprintf(configuration.Name, std::size(configuration.Name), "%.*s%.2f",
        static_cast<int>(kPlatformFontNamePrefix.size()), kPlatformFontNamePrefix.data(), scale);
}

bool IsPlatformFont(const ImFont& font, const float scale) noexcept {
    char expected[40]{};
    std::snprintf(expected, std::size(expected), "%.*s%.2f",
        static_cast<int>(kPlatformFontNamePrefix.size()), kPlatformFontNamePrefix.data(), scale);
    return std::strcmp(font.GetDebugName(), expected) == 0;
}

const ImWchar* PlatformChineseGlyphRanges(ImFontAtlas& atlas) noexcept {
    // Host and bundled-plugin text outside ImGui's common 2500 glyphs.
    static constexpr ImWchar kSupplementalRanges[] = {
        0x4F59, 0x4F59,
        0x5149, 0x5149,
        0x55B5, 0x55B5,
        0x5E27, 0x5E27,
        0x5ED3, 0x5ED3,
        0x5F20, 0x5F20,
        0x62DF, 0x62DF,
        0x6781, 0x6781,
        0x6D4F, 0x6D4F,
        0x70EC, 0x70EC,
        0x76C2, 0x76C2,
        0x7948, 0x7948,
        0x7EB8, 0x7EB8,
        0x7EEF, 0x7EEF,
        0x7FE1, 0x7FE1,
        0x82D4, 0x82D4,
        0x83B9, 0x83B9,
        0x85D3, 0x85D3,
        0x8D26, 0x8D26,
        0x8F91, 0x8F91,
        0x91C9, 0x91C9,
        0x938F, 0x938F,
        0x9608, 0x9608,
        0x9891, 0x9891,
        0x9EDB, 0x9EDB,
        0,
    };
    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(atlas.GetGlyphRangesChineseSimplifiedCommon());
        builder.AddRanges(kSupplementalRanges);
        builder.BuildRanges(&ranges);
    }
    return ranges.Data;
}

}  // namespace

void SetPlatformUiPalette(const PlatformUiPalette palette) noexcept {
    g_platform_ui_palette.store(palette, std::memory_order_relaxed);
}

PlatformUiPalette GetPlatformUiPalette() noexcept {
    return g_platform_ui_palette.load(std::memory_order_relaxed);
}

void SetPlatformUiCustomColors(const PlatformUiCustomColors& colors) noexcept {
    g_platform_ui_custom_colors = colors;
    g_platform_ui_custom_theme = BuildCustomTheme(colors);
}

const PlatformUiCustomColors& GetPlatformUiCustomColors() noexcept {
    return g_platform_ui_custom_colors;
}

const PlatformUiThemeColors& PlatformUiTheme() noexcept {
    switch (GetPlatformUiPalette()) {
    case PlatformUiPalette::Aurora: return kAurora;
    case PlatformUiPalette::Ember: return kEmber;
    case PlatformUiPalette::Paper: return kPaper;
    case PlatformUiPalette::AnomalyHub: return kAnomalyHub;
    case PlatformUiPalette::Custom: return g_platform_ui_custom_theme;
    case PlatformUiPalette::Moss: return kMoss;
    }
    return kAnomalyHub;
}

PlatformUiPalette ParsePlatformUiPalette(const std::string_view value) noexcept {
    if (value == "aurora") return PlatformUiPalette::Aurora;
    if (value == "ember") return PlatformUiPalette::Ember;
    if (value == "paper") return PlatformUiPalette::Paper;
    if (value == "anomalyhub") return PlatformUiPalette::AnomalyHub;
    if (value == "custom") return PlatformUiPalette::Custom;
    return PlatformUiPalette::AnomalyHub;
}

std::string_view ToString(const PlatformUiPalette palette) noexcept {
    switch (palette) {
    case PlatformUiPalette::Aurora: return "aurora";
    case PlatformUiPalette::Ember: return "ember";
    case PlatformUiPalette::Paper: return "paper";
    case PlatformUiPalette::AnomalyHub: return "anomalyhub";
    case PlatformUiPalette::Custom: return "custom";
    case PlatformUiPalette::Moss: return "moss";
    }
    return "anomalyhub";
}

namespace {

ImVec4 ImGuiColor(const PlatformUiColor& color) noexcept {
    return {color.red, color.green, color.blue, color.alpha};
}

}  // namespace

void ApplyPlatformUiStyle() noexcept {
    ImGuiStyle& style = ImGui::GetStyle();
    const PlatformUiThemeColors& theme = PlatformUiTheme();
    style.WindowRounding = 4.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 3.0f;
    // FrameBg matches ChildBg, so without a frame border an unchecked checkbox
    // (and other empty frames) is indistinguishable from its surroundings.
    style.FrameBorderSize = 1.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 3.0f;
    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 6.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.IndentSpacing = 16.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;
    auto* colors = style.Colors;
    colors[ImGuiCol_Text] = ImGuiColor(theme.text);
    colors[ImGuiCol_TextDisabled] = ImGuiColor(theme.text_muted);
    colors[ImGuiCol_WindowBg] = ImGuiColor(theme.window_background);
    colors[ImGuiCol_ChildBg] = ImGuiColor(theme.child_background);
    colors[ImGuiCol_PopupBg] = ImGuiColor(theme.popup_background);
    colors[ImGuiCol_Border] = ImGuiColor(theme.border);
    colors[ImGuiCol_FrameBg] = ImGuiColor(theme.frame);
    colors[ImGuiCol_FrameBgHovered] = ImGuiColor(theme.frame_hovered);
    colors[ImGuiCol_FrameBgActive] = ImGuiColor(theme.frame_active);
    colors[ImGuiCol_TitleBg] = ImGuiColor(theme.navigation_background);
    colors[ImGuiCol_TitleBgActive] = ImGuiColor(theme.window_background);
    colors[ImGuiCol_Button] = ImGuiColor(theme.button);
    colors[ImGuiCol_ButtonHovered] = ImGuiColor(theme.button_hovered);
    colors[ImGuiCol_ButtonActive] = ImGuiColor(theme.button_active);
    colors[ImGuiCol_Header] = ImGuiColor(theme.accent_soft);
    colors[ImGuiCol_HeaderHovered] = ImVec4(
        theme.accent.red, theme.accent.green, theme.accent.blue, 0.24f);
    colors[ImGuiCol_HeaderActive] = ImVec4(
        theme.accent.red, theme.accent.green, theme.accent.blue, 0.32f);
    colors[ImGuiCol_CheckMark] = ImGuiColor(theme.accent);
    colors[ImGuiCol_SliderGrab] = ImGuiColor(theme.accent);
    colors[ImGuiCol_SliderGrabActive] = ImGuiColor(theme.accent_hovered);
    colors[ImGuiCol_Separator] = colors[ImGuiCol_Border];
    colors[ImGuiCol_ScrollbarBg] = ImGuiColor(theme.navigation_background);
    colors[ImGuiCol_ScrollbarGrab] = ImGuiColor(theme.border);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImGuiColor(theme.button_hovered);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(
        theme.accent.red, theme.accent.green, theme.accent.blue, 0.70f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(
        theme.accent.red, theme.accent.green, theme.accent.blue, 0.0f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(
        theme.accent.red, theme.accent.green, theme.accent.blue, 0.45f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(
        theme.accent.red, theme.accent.green, theme.accent.blue, 0.80f);
    colors[ImGuiCol_Tab] = ImGuiColor(theme.window_background);
    colors[ImGuiCol_TabHovered] = ImGuiColor(theme.button_hovered);
    colors[ImGuiCol_TabSelected] = ImGuiColor(theme.child_background);
    colors[ImGuiCol_TableHeaderBg] = ImGuiColor(theme.child_background);
    colors[ImGuiCol_TableBorderStrong] = colors[ImGuiCol_Border];
    colors[ImGuiCol_TableBorderLight] = ImVec4(
        theme.border.red, theme.border.green, theme.border.blue, 0.55f);
}

bool ConfigurePlatformUiFontAtlas(const std::filesystem::path& runtime_root) noexcept {
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts == nullptr) return false;
    io.FontDefault = nullptr;
    io.Fonts->Clear();
    ImFontConfig cjk_configuration;
    static constexpr ImWchar icon_range[] = {0xE700, 0xF8FF, 0};
    bool complete = true;
    for (const float scale : kPlatformFontBakeScales) {
        ImFontConfig configuration;
        configuration.OversampleH = 2;
        configuration.OversampleV = 1;
        SetPlatformFontName(configuration, scale);
        ImFont* const font = io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\segoeui.ttf",
            kPlatformFontSizePixels * scale, &configuration);
        if (io.FontDefault == nullptr && font != nullptr) io.FontDefault = font;
        if (font == nullptr) {
            complete = false;
            continue;
        }

        cjk_configuration = {};
        cjk_configuration.MergeMode = true;
        cjk_configuration.PixelSnapH = true;
        if (AddCachedFontFromPath(*io.Fonts,
                runtime_root / L"assets" / L"fonts" / L"NotoSansCJKsc-Regular.ttf",
                kPlatformFontSizePixels * scale, cjk_configuration,
                PlatformChineseGlyphRanges(*io.Fonts)) == nullptr) {
            complete = false;
        }

        ImFontConfig icon_configuration;
        icon_configuration.MergeMode = true;
        icon_configuration.PixelSnapH = true;
        icon_configuration.GlyphOffset = ImVec2(0.0f, 2.0f * scale);
        icon_configuration.GlyphMinAdvanceX = kPlatformFontSizePixels * scale;
        if (io.Fonts->AddFontFromFileTTF(
                "C:\\Windows\\Fonts\\segmdl2.ttf", 14.0f * scale,
                &icon_configuration, icon_range) == nullptr) {
            complete = false;
        }
    }
    if (io.FontDefault == nullptr) {
        io.FontDefault = io.Fonts->AddFontDefault();
        complete = false;
    }
    return complete && ApplyPlatformUiFontScale(1.0f);
}

bool ApplyPlatformUiFontScale(const float scale) noexcept {
    if (!std::isfinite(scale) || scale <= 0.0f) return false;
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts == nullptr) return false;
    float selected_scale = kPlatformFontBakeScales.front();
    float smallest_distance = (std::numeric_limits<float>::max)();
    for (const float baked_scale : kPlatformFontBakeScales) {
        const float distance = std::abs(scale - baked_scale);
        if (distance < smallest_distance) {
            selected_scale = baked_scale;
            smallest_distance = distance;
        }
    }
    ImFont* selected = io.FontDefault != nullptr &&
            IsPlatformFont(*io.FontDefault, selected_scale)
        ? io.FontDefault
        : nullptr;
    if (selected == nullptr) {
        for (ImFont* const font : io.Fonts->Fonts) {
            if (font != nullptr && IsPlatformFont(*font, selected_scale)) {
                selected = font;
                break;
            }
        }
    }
    if (selected == nullptr) return false;
    io.FontDefault = selected;
    selected->Scale = selected->FontSize > 0.0f
        ? kPlatformFontSizePixels / selected->FontSize
        : 1.0f / selected_scale;
    io.FontGlobalScale = scale;
    return true;
}

}  // namespace ue5mem
