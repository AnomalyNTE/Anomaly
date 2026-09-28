#pragma once

#include "anomaly/platform_ui_theme.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

// A theme layout is the user-editable half of a theme: one background image and
// any number of stickers, all expressed in normalized viewport coordinates so a
// single file drives both the launcher and the in-game shell at any size.
//
// The files live under config/themes inside the runtime root and are
// produced by tools/theme_layout_editor. Nothing here depends on ImGui, D3D or
// file I/O beyond the path resolver, so the geometry can be verified on its own.
inline constexpr std::uint32_t kPlatformUiLayoutSchemaVersion = 1;
// Layouts live under config/themes. "layout.json" is the shared layout and
// applies to every palette; "<palette>.json" overrides it for that palette only,
// so one artwork can serve all themes while a themed one takes precedence.
inline constexpr std::string_view kPlatformUiLayoutDirectory = "config/themes";
inline constexpr std::string_view kPlatformUiLayoutSharedFileName = "layout.json";
inline constexpr std::string_view kPlatformUiLayoutRelativePath = "config/themes/layout.json";
inline constexpr float kPlatformUiLayoutDefaultCanvasWidth = 1180.0f;
inline constexpr float kPlatformUiLayoutDefaultCanvasHeight = 700.0f;
inline constexpr std::size_t kPlatformUiLayoutMaximumStickers = 64;
inline constexpr std::size_t kPlatformUiLayoutMaximumPathLength = 512;
inline constexpr std::size_t kPlatformUiLayoutMaximumBytes = 256U * 1024U;
inline constexpr std::size_t kPlatformUiLayoutMaximumPaletteLength = 32;
inline constexpr float kPlatformUiLayoutSurfaceOpacityMinimum = 0.0f;
// The veil a layout gets when a wallpaper is turned on from the settings page.
// It keeps the shell readable while still letting the picture through: a fully
// opaque surface would hide the very image the user just chose.
inline constexpr float kPlatformUiLayoutSurfaceOpacityDefault = 0.72f;

// Center keeps the whole image visible (uniform scale, letterboxed), Crop fills
// the viewport and trims the overflow, Stretch fills the viewport and allows
// distortion. Disabled draws nothing.
enum class PlatformUiBackgroundMode : std::uint8_t {
    Disabled,
    Center,
    Crop,
    Stretch,
};

enum class PlatformUiAnchor : std::uint8_t {
    Center,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

enum class PlatformUiStickerLayer : std::uint8_t {
    Behind,
    Front,
};

struct PlatformUiBackground final {
    std::string path;
    PlatformUiBackgroundMode mode{PlatformUiBackgroundMode::Disabled};
    float opacity{1.0f};
    PlatformUiColor tint{1.0f, 1.0f, 1.0f, 1.0f};

    friend bool operator==(const PlatformUiBackground&, const PlatformUiBackground&) = default;
};

struct PlatformUiSticker final {
    std::string path;
    // Anchor position in normalized viewport coordinates.
    float x{0.5f};
    float y{0.5f};
    // Normalized size. Zero means "derive from the other axis and the image
    // aspect"; both zero means the image's native pixel size.
    float width{};
    float height{};
    float rotation{};
    float opacity{1.0f};
    PlatformUiAnchor anchor{PlatformUiAnchor::Center};
    PlatformUiStickerLayer layer{PlatformUiStickerLayer::Behind};
    PlatformUiColor tint{1.0f, 1.0f, 1.0f, 1.0f};
    bool flip_x{};
    bool flip_y{};

    friend bool operator==(const PlatformUiSticker&, const PlatformUiSticker&) = default;
};

struct PlatformUiLayoutDocument final {
    std::uint32_t schema_version{kPlatformUiLayoutSchemaVersion};
    float canvas_width{kPlatformUiLayoutDefaultCanvasWidth};
    float canvas_height{kPlatformUiLayoutDefaultCanvasHeight};
    // Multiplied into every surface token while this layout is active, so the
    // background image shows through the shell panels.
    float surface_opacity{1.0f};
    PlatformUiBackground background;
    std::vector<PlatformUiSticker> stickers;

    [[nodiscard]] bool Empty() const noexcept {
        return stickers.empty() &&
            (background.path.empty() || background.mode == PlatformUiBackgroundMode::Disabled);
    }
    friend bool operator==(const PlatformUiLayoutDocument&, const PlatformUiLayoutDocument&) = default;
};

struct PlatformUiLayoutParseResult final {
    bool ok{};
    std::string message;
    PlatformUiLayoutDocument layout;
};

[[nodiscard]] PlatformUiLayoutParseResult ParsePlatformUiLayout(std::string_view json);

// Serializes a layout back to the documented schema, emitting only schema fields
// in a stable order so a file written by the shell stays diffable against one
// exported by the editor. A layout with no background omits that block entirely
// instead of writing a placeholder, and the parser accepts such a file.
[[nodiscard]] std::string SerializePlatformUiLayout(const PlatformUiLayoutDocument& layout);

// Picks the layout file for one palette: config/themes/<palette>.json when it
// exists, otherwise the shared config/themes/layout.json. Returns the shared
// path when neither exists, so callers always have a well-defined target to
// report and to write settings into.
[[nodiscard]] std::filesystem::path ResolvePlatformUiLayoutFile(
    const std::filesystem::path& runtime_root, std::string_view palette) noexcept;

// Writes a layout atomically, because the theme worker polls this file's stamp
// and must never observe a half-written document. `error` receives a message
// suitable for the settings page when the write fails.
[[nodiscard]] bool WritePlatformUiLayout(
    const std::filesystem::path& path, const PlatformUiLayoutDocument& layout,
    std::string& error) noexcept;

[[nodiscard]] std::string_view ToString(PlatformUiBackgroundMode mode) noexcept;
[[nodiscard]] PlatformUiBackgroundMode ParsePlatformUiBackgroundMode(std::string_view value) noexcept;
[[nodiscard]] std::string_view ToString(PlatformUiAnchor anchor) noexcept;
[[nodiscard]] PlatformUiAnchor ParsePlatformUiAnchor(std::string_view value) noexcept;
[[nodiscard]] std::string_view ToString(PlatformUiStickerLayer layer) noexcept;
[[nodiscard]] PlatformUiStickerLayer ParsePlatformUiStickerLayer(std::string_view value) noexcept;

// Resolves a layout path against the runtime root. Absolute paths, drive
// letters, ".." segments and empty values are rejected: the layout file is
// user-editable data and must not be able to point the decoder at arbitrary
// files.
[[nodiscard]] bool ResolvePlatformUiLayoutPath(
    const std::filesystem::path& runtime_root, std::string_view relative_path,
    std::filesystem::path& resolved) noexcept;

// Destination rectangle in screen space plus the normalized source rectangle
// sampled from the image.
struct PlatformUiImagePlacement final {
    float x{};
    float y{};
    float width{};
    float height{};
    float uv0_x{};
    float uv0_y{};
    float uv1_x{1.0f};
    float uv1_y{1.0f};

    [[nodiscard]] bool Visible() const noexcept {
        return width > 0.0f && height > 0.0f;
    }
};

[[nodiscard]] PlatformUiImagePlacement ComputePlatformUiBackgroundPlacement(
    PlatformUiBackgroundMode mode, float image_width, float image_height,
    float viewport_x, float viewport_y, float viewport_width,
    float viewport_height) noexcept;

// Viewport-relative placement. x/y are the unrotated top-left corner.
struct PlatformUiStickerPlacement final {
    float x{};
    float y{};
    float width{};
    float height{};
    float rotation{};
    float uv0_x{};
    float uv0_y{};
    float uv1_x{1.0f};
    float uv1_y{1.0f};
    float alpha{1.0f};

    [[nodiscard]] bool Visible() const noexcept {
        return width > 0.0f && height > 0.0f && alpha > 0.0f;
    }
};

[[nodiscard]] PlatformUiStickerPlacement ComputePlatformUiStickerPlacement(
    const PlatformUiSticker& sticker, float image_width, float image_height,
    float viewport_width, float viewport_height) noexcept;

// Corners in ImGui's AddImageQuad order: top-left, top-right, bottom-right,
// bottom-left, rotated clockwise around the rectangle centre. uv0/uv1 map to
// the first and second corner; the remaining corners interpolate, which keeps a
// flipped source rectangle correct.
struct PlatformUiImageQuad final {
    float x[4]{};
    float y[4]{};
    float u[4]{};
    float v[4]{};
};

[[nodiscard]] PlatformUiImageQuad ComputePlatformUiImageQuad(
    const PlatformUiStickerPlacement& placement) noexcept;

}  // namespace anomaly
