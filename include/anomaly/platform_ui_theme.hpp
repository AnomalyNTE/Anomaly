#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

struct ImFont;

namespace ue5mem {

enum class PlatformUiPalette : std::uint8_t {
    Moss,
    Aurora,
    Ember,
    Paper,
    AnomalyHub,
    Naiwa,
    Custom,
};

struct PlatformUiColor final {
    float red;
    float green;
    float blue;
    float alpha;

    friend bool operator==(const PlatformUiColor&, const PlatformUiColor&) = default;
};

struct PlatformUiCustomColors final {
    PlatformUiColor accent{1.000f, 0.639f, 0.102f, 1.0f};
    PlatformUiColor text{0.950f, 0.950f, 0.950f, 1.0f};
    PlatformUiColor window_background{0.106f, 0.106f, 0.106f, 1.0f};
    PlatformUiColor child_background{0.161f, 0.161f, 0.161f, 1.0f};
    PlatformUiColor border{0.350f, 0.350f, 0.350f, 1.0f};

    friend bool operator==(const PlatformUiCustomColors&, const PlatformUiCustomColors&) = default;
};

// Shared color tokens used by the ImGui style and custom platform surfaces.
struct PlatformUiThemeColors final {
    PlatformUiColor text;
    PlatformUiColor text_muted;
    PlatformUiColor window_background;
    PlatformUiColor child_background;
    PlatformUiColor popup_background;
    PlatformUiColor border;
    PlatformUiColor frame;
    PlatformUiColor frame_hovered;
    PlatformUiColor frame_active;
    PlatformUiColor button;
    PlatformUiColor button_hovered;
    PlatformUiColor button_active;
    PlatformUiColor accent;
    PlatformUiColor accent_hovered;
    PlatformUiColor accent_active;
    PlatformUiColor accent_soft;
    PlatformUiColor success;
    PlatformUiColor warning;
    PlatformUiColor danger;
    PlatformUiColor info;
    PlatformUiColor inverse_text;
    PlatformUiColor navigation_background;
    PlatformUiColor header_background;
    PlatformUiColor toolbar_background;
    PlatformUiColor panel_background;
    PlatformUiColor row_hovered;
    PlatformUiColor toggle_off;
    PlatformUiColor toggle_off_border;
    PlatformUiColor toggle_on_knob;
    PlatformUiColor icon_fill;
    PlatformUiColor icon_border;
    PlatformUiColor toast_background;
};

// A themed background image is painted behind the shell surfaces, and the host
// draws one veil of the surface colour over it at this alpha: that veil is what
// dims the wallpaper, while text, borders and accents stay fully opaque. 1.0
// disables the effect and restores the palette exactly, 0.0 shows the picture at
// full strength. Panel fills stay empty either way, so they never cover a
// sticker.
inline constexpr float kPlatformUiSurfaceAlphaMinimum = 0.0f;

void SetPlatformUiPalette(PlatformUiPalette palette) noexcept;
[[nodiscard]] PlatformUiPalette GetPlatformUiPalette() noexcept;
void SetPlatformUiCustomColors(const PlatformUiCustomColors& colors) noexcept;
[[nodiscard]] const PlatformUiCustomColors& GetPlatformUiCustomColors() noexcept;
void SetPlatformUiSurfaceAlpha(float alpha) noexcept;
[[nodiscard]] float GetPlatformUiSurfaceAlpha() noexcept;
// The surface color at full opacity, for the veil the host paints under the
// stickers and for windows that must keep a solid body while the shell itself
// is drawn over a themed background.
[[nodiscard]] PlatformUiColor PlatformUiSurfaceColor() noexcept;
[[nodiscard]] const PlatformUiThemeColors& PlatformUiTheme() noexcept;
[[nodiscard]] PlatformUiPalette ParsePlatformUiPalette(std::string_view value) noexcept;
[[nodiscard]] std::string_view ToString(PlatformUiPalette palette) noexcept;

// A font bake narrower than the host's. The host bakes a ladder of sizes and the
// whole MDL2 icon range because in-game windows draw plugin text, localized
// documents and icons that are not known ahead of time. A surface whose type scale
// and icons are fixed -- a standalone window that draws its own chrome -- passes
// exactly what it draws instead, and pays for one baked size and a handful of icons
// rather than for all of them. Glyph coverage is the same either way, so text the
// surface does not author (paths, process names, localized messages) still renders.
struct PlatformUiFontBake final {
    // Sizes as multiples of PlatformUiBaseFontSize(). Empty bakes the host ladder.
    std::span<const float> scales{};
    // Icon glyphs from the icon font. Empty bakes the whole icon range.
    std::span<const char32_t> icons{};
    // Lets the atlas keep a height that is not rounded up to a power of two, which is
    // the empty band under the last row of glyphs. The glyph pixels and the texture
    // coordinates derived from them are the same either way.
    bool tight_atlas{false};
};

// Requires an active ImGui context.
void ApplyPlatformUiStyle() noexcept;
[[nodiscard]] bool ConfigurePlatformUiFontAtlas(
    const std::filesystem::path& runtime_root = {}) noexcept;
[[nodiscard]] bool ConfigurePlatformUiFontAtlas(
    const std::filesystem::path& runtime_root, const PlatformUiFontBake& bake) noexcept;
[[nodiscard]] bool ApplyPlatformUiFontScale(float scale) noexcept;
// The smallest baked host font whose pixel size is at least `scale` times the
// base size, so large titles and icons are drawn from a crisp bake instead of
// a magnified one. Draw with ImDrawList::AddText(font, size, ...).
[[nodiscard]] ImFont* PlatformUiFontForScale(float scale) noexcept;
[[nodiscard]] float PlatformUiBaseFontSize() noexcept;
// What the atlas still needs after its texture is uploaded. The CPU-side pixels are
// five times the atlas area and nothing draws from them again either way.
//   Rebuildable  keeps the sources, so a lost device can upload the texture again.
//   Minimal      drops them too: the glyphs are already rasterized into the texture,
//                and the font files are what the atlas costs besides it. Only for a
//                surface that never rebuilds its atlas -- its device lives as long as
//                it does, and no font is added after this call.
enum class PlatformUiFontRetention : std::uint8_t { Rebuildable, Minimal };

void ReleasePlatformUiFontAtlas(PlatformUiFontRetention retention) noexcept;

}  // namespace ue5mem

// The platform host model lives in the anomaly namespace. Keep a small alias
// surface there so custom host drawing can consume the same theme tokens.
namespace anomaly {
using PlatformUiPalette = ue5mem::PlatformUiPalette;
using PlatformUiColor = ue5mem::PlatformUiColor;
using PlatformUiCustomColors = ue5mem::PlatformUiCustomColors;
using PlatformUiThemeColors = ue5mem::PlatformUiThemeColors;

inline void SetPlatformUiPalette(const PlatformUiPalette palette) noexcept {
    ue5mem::SetPlatformUiPalette(palette);
}

[[nodiscard]] inline PlatformUiPalette GetPlatformUiPalette() noexcept {
    return ue5mem::GetPlatformUiPalette();
}

inline void SetPlatformUiCustomColors(const PlatformUiCustomColors& colors) noexcept {
    ue5mem::SetPlatformUiCustomColors(colors);
}

[[nodiscard]] inline const PlatformUiCustomColors& GetPlatformUiCustomColors() noexcept {
    return ue5mem::GetPlatformUiCustomColors();
}

inline void SetPlatformUiSurfaceAlpha(const float alpha) noexcept {
    ue5mem::SetPlatformUiSurfaceAlpha(alpha);
}

[[nodiscard]] inline float GetPlatformUiSurfaceAlpha() noexcept {
    return ue5mem::GetPlatformUiSurfaceAlpha();
}

[[nodiscard]] inline ue5mem::PlatformUiColor PlatformUiSurfaceColor() noexcept {
    return ue5mem::PlatformUiSurfaceColor();
}

[[nodiscard]] inline const PlatformUiThemeColors& PlatformUiTheme() noexcept {
    return ue5mem::PlatformUiTheme();
}

[[nodiscard]] inline PlatformUiPalette ParsePlatformUiPalette(
    const std::string_view value) noexcept {
    return ue5mem::ParsePlatformUiPalette(value);
}

[[nodiscard]] inline std::string_view ToString(const PlatformUiPalette palette) noexcept {
    return ue5mem::ToString(palette);
}
}  // namespace anomaly
