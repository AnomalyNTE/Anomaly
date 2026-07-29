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

namespace ue5mem {
namespace {

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
        0x55B5, 0x55B5,
        0x5E27, 0x5E27,
        0x5ED3, 0x5ED3,
        0x62DF, 0x62DF,
        0x6D4F, 0x6D4F,
        0x76C2, 0x76C2,
        0x7948, 0x7948,
        0x7EEF, 0x7EEF,
        0x7FE1, 0x7FE1,
        0x83B9, 0x83B9,
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

void ApplyPlatformUiStyle() noexcept {
    ImGuiStyle& style = ImGui::GetStyle();
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
    colors[ImGuiCol_Text] = ImVec4(0.949f, 0.957f, 0.965f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.451f, 0.490f, 0.533f, 1.0f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.078f, 0.090f, 0.102f, 1.0f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.106f, 0.125f, 0.145f, 1.0f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.137f, 0.165f, 0.192f, 1.0f);
    colors[ImGuiCol_Border] = ImVec4(0.204f, 0.235f, 0.271f, 1.0f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.106f, 0.125f, 0.145f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.137f, 0.165f, 0.192f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.150f, 0.180f, 0.208f, 1.0f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.063f, 0.075f, 0.090f, 1.0f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.078f, 0.090f, 0.102f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.106f, 0.125f, 0.145f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.137f, 0.165f, 0.192f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.180f, 0.260f, 0.255f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.345f, 0.718f, 0.647f, 0.16f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.345f, 0.718f, 0.647f, 0.24f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.345f, 0.718f, 0.647f, 0.32f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.345f, 0.718f, 0.647f, 1.0f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.345f, 0.718f, 0.647f, 1.0f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.380f, 0.788f, 0.690f, 1.0f);
    colors[ImGuiCol_Separator] = colors[ImGuiCol_Border];
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.063f, 0.075f, 0.090f, 1.0f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.204f, 0.235f, 0.271f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.275f, 0.322f, 0.365f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.345f, 0.718f, 0.647f, 0.70f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.345f, 0.718f, 0.647f, 0.0f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.345f, 0.718f, 0.647f, 0.45f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.345f, 0.718f, 0.647f, 0.80f);
    colors[ImGuiCol_Tab] = ImVec4(0.078f, 0.090f, 0.102f, 1.0f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.137f, 0.165f, 0.192f, 1.0f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.106f, 0.125f, 0.145f, 1.0f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.106f, 0.125f, 0.145f, 1.0f);
    colors[ImGuiCol_TableBorderStrong] = colors[ImGuiCol_Border];
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.204f, 0.235f, 0.271f, 0.55f);
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
