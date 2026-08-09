#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace ue5mem {

enum class PlatformUiPalette : std::uint8_t {
    Moss,
    Aurora,
    Ember,
    Paper,
    AnomalyHub,
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

void SetPlatformUiPalette(PlatformUiPalette palette) noexcept;
[[nodiscard]] PlatformUiPalette GetPlatformUiPalette() noexcept;
void SetPlatformUiCustomColors(const PlatformUiCustomColors& colors) noexcept;
[[nodiscard]] const PlatformUiCustomColors& GetPlatformUiCustomColors() noexcept;
[[nodiscard]] const PlatformUiThemeColors& PlatformUiTheme() noexcept;
[[nodiscard]] PlatformUiPalette ParsePlatformUiPalette(std::string_view value) noexcept;
[[nodiscard]] std::string_view ToString(PlatformUiPalette palette) noexcept;

// Requires an active ImGui context.
void ApplyPlatformUiStyle() noexcept;
[[nodiscard]] bool ConfigurePlatformUiFontAtlas(
    const std::filesystem::path& runtime_root = {}) noexcept;
[[nodiscard]] bool ApplyPlatformUiFontScale(float scale) noexcept;

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
