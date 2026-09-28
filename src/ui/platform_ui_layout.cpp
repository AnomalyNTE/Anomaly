#include "anomaly/platform_ui_layout.hpp"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

namespace anomaly {
namespace {

constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;

[[nodiscard]] bool IsUsableDimension(const float value) noexcept {
    return std::isfinite(value) && value > 0.0f;
}

[[nodiscard]] float ClampUnit(const float value, const float fallback) noexcept {
    if (!std::isfinite(value)) return fallback;
    return std::clamp(value, 0.0f, 1.0f);
}

[[nodiscard]] float ClampFinite(const float value, const float minimum, const float maximum,
                                const float fallback) noexcept {
    if (!std::isfinite(value)) return fallback;
    return std::clamp(value, minimum, maximum);
}

// Layout paths are UTF-8 in the file and native wide on disk.
[[nodiscard]] std::wstring Utf8ToWide(const std::string_view text) {
    if (text.empty()) return {};
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) throw std::invalid_argument("layout path is not valid UTF-8");
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), result.data(), required) != required) {
        throw std::invalid_argument("layout path is not valid UTF-8");
    }
    return result;
}

[[nodiscard]] PlatformUiColor ParseHexColor(
    const std::string& text, const PlatformUiColor& fallback) noexcept {
    if (text.size() != 7 || text.front() != '#') return fallback;
    unsigned int channels[3]{};
    for (std::size_t index = 0; index < 3; ++index) {
        unsigned int value{};
        for (std::size_t digit = 0; digit < 2; ++digit) {
            const char character = text[1 + index * 2 + digit];
            unsigned int nibble{};
            if (character >= '0' && character <= '9') {
                nibble = static_cast<unsigned int>(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                nibble = static_cast<unsigned int>(character - 'a' + 10);
            } else if (character >= 'A' && character <= 'F') {
                nibble = static_cast<unsigned int>(character - 'A' + 10);
            } else {
                return fallback;
            }
            value = value * 16U + nibble;
        }
        channels[index] = value;
    }
    return {
        static_cast<float>(channels[0]) / 255.0f,
        static_cast<float>(channels[1]) / 255.0f,
        static_cast<float>(channels[2]) / 255.0f,
        1.0f};
}

[[nodiscard]] std::string ReadPath(const nlohmann::json& value) {
    const auto found = value.find("path");
    if (found == value.end() || !found->is_string()) return {};
    std::string path = found->get<std::string>();
    if (path.size() > kPlatformUiLayoutMaximumPathLength) {
        throw std::invalid_argument("layout path is longer than the supported limit");
    }
    return path;
}

[[nodiscard]] PlatformUiColor ReadTint(const nlohmann::json& value) {
    const auto found = value.find("tint");
    if (found == value.end() || !found->is_string()) return {1.0f, 1.0f, 1.0f, 1.0f};
    return ParseHexColor(found->get<std::string>(), {1.0f, 1.0f, 1.0f, 1.0f});
}

[[nodiscard]] float ReadFloat(const nlohmann::json& value, const char* id,
                              const float fallback) {
    const auto found = value.find(id);
    if (found == value.end() || !found->is_number()) return fallback;
    return found->get<float>();
}

[[nodiscard]] bool ReadBool(const nlohmann::json& value, const char* id,
                            const bool fallback) {
    const auto found = value.find(id);
    if (found == value.end() || !found->is_boolean()) return fallback;
    return found->get<bool>();
}

[[nodiscard]] std::string ReadString(const nlohmann::json& value, const char* id,
                                     const std::string_view fallback) {
    const auto found = value.find(id);
    if (found == value.end() || !found->is_string()) return std::string(fallback);
    return found->get<std::string>();
}

[[nodiscard]] PlatformUiBackground ParseBackground(const nlohmann::json& document) {
    PlatformUiBackground background;
    const auto found = document.find("background");
    if (found == document.end() || !found->is_object()) return background;
    const nlohmann::json& value = *found;
    background.path = ReadPath(value);
    background.mode = ParsePlatformUiBackgroundMode(
        ReadString(value, "mode", ToString(PlatformUiBackgroundMode::Disabled)));
    background.opacity = ClampUnit(ReadFloat(value, "opacity", 1.0f), 1.0f);
    background.tint = ReadTint(value);
    return background;
}

[[nodiscard]] std::vector<PlatformUiSticker> ParseStickers(const nlohmann::json& document) {
    std::vector<PlatformUiSticker> stickers;
    const auto found = document.find("stickers");
    if (found == document.end()) return stickers;
    if (!found->is_array()) throw std::invalid_argument("stickers must be an array");
    if (found->size() > kPlatformUiLayoutMaximumStickers) {
        throw std::invalid_argument("too many stickers in the layout");
    }
    stickers.reserve(found->size());
    for (const nlohmann::json& value : *found) {
        if (!value.is_object()) throw std::invalid_argument("every sticker must be an object");
        PlatformUiSticker sticker;
        sticker.path = ReadPath(value);
        // A sticker without a texture cannot be drawn, so an entry that names no
        // file is dropped rather than kept as an invisible placeholder.
        if (sticker.path.empty()) continue;
        sticker.x = ClampFinite(ReadFloat(value, "x", 0.5f), -4.0f, 5.0f, 0.5f);
        sticker.y = ClampFinite(ReadFloat(value, "y", 0.5f), -4.0f, 5.0f, 0.5f);
        sticker.width = ClampFinite(ReadFloat(value, "width", 0.0f), 0.0f, 8.0f, 0.0f);
        sticker.height = ClampFinite(ReadFloat(value, "height", 0.0f), 0.0f, 8.0f, 0.0f);
        sticker.rotation = ClampFinite(ReadFloat(value, "rotation", 0.0f), -3600.0f, 3600.0f, 0.0f);
        sticker.opacity = ClampUnit(ReadFloat(value, "opacity", 1.0f), 1.0f);
        sticker.anchor = ParsePlatformUiAnchor(
            ReadString(value, "anchor", ToString(PlatformUiAnchor::Center)));
        sticker.layer = ParsePlatformUiStickerLayer(
            ReadString(value, "layer", ToString(PlatformUiStickerLayer::Behind)));
        sticker.tint = ReadTint(value);
        sticker.flip_x = ReadBool(value, "flipX", false);
        sticker.flip_y = ReadBool(value, "flipY", false);
        stickers.push_back(std::move(sticker));
    }
    return stickers;
}

}  // namespace

std::string_view ToString(const PlatformUiBackgroundMode mode) noexcept {
    switch (mode) {
    case PlatformUiBackgroundMode::Disabled: return "disabled";
    case PlatformUiBackgroundMode::Center: return "center";
    case PlatformUiBackgroundMode::Crop: return "crop";
    case PlatformUiBackgroundMode::Stretch: return "stretch";
    }
    return "disabled";
}

PlatformUiBackgroundMode ParsePlatformUiBackgroundMode(const std::string_view value) noexcept {
    if (value == "center") return PlatformUiBackgroundMode::Center;
    if (value == "crop") return PlatformUiBackgroundMode::Crop;
    if (value == "stretch") return PlatformUiBackgroundMode::Stretch;
    return PlatformUiBackgroundMode::Disabled;
}

std::string_view ToString(const PlatformUiAnchor anchor) noexcept {
    switch (anchor) {
    case PlatformUiAnchor::Center: return "center";
    case PlatformUiAnchor::TopLeft: return "top_left";
    case PlatformUiAnchor::TopRight: return "top_right";
    case PlatformUiAnchor::BottomLeft: return "bottom_left";
    case PlatformUiAnchor::BottomRight: return "bottom_right";
    }
    return "center";
}

PlatformUiAnchor ParsePlatformUiAnchor(const std::string_view value) noexcept {
    if (value == "top_left") return PlatformUiAnchor::TopLeft;
    if (value == "top_right") return PlatformUiAnchor::TopRight;
    if (value == "bottom_left") return PlatformUiAnchor::BottomLeft;
    if (value == "bottom_right") return PlatformUiAnchor::BottomRight;
    return PlatformUiAnchor::Center;
}

std::string_view ToString(const PlatformUiStickerLayer layer) noexcept {
    switch (layer) {
    case PlatformUiStickerLayer::Behind: return "behind";
    case PlatformUiStickerLayer::Front: return "front";
    }
    return "behind";
}

PlatformUiStickerLayer ParsePlatformUiStickerLayer(const std::string_view value) noexcept {
    return value == "front" ? PlatformUiStickerLayer::Front : PlatformUiStickerLayer::Behind;
}

PlatformUiLayoutParseResult ParsePlatformUiLayout(const std::string_view json) {
    PlatformUiLayoutParseResult result;
    if (json.empty()) {
        result.message = "the layout file is empty";
        return result;
    }
    if (json.size() > kPlatformUiLayoutMaximumBytes) {
        result.message = "the layout file is larger than the supported limit";
        return result;
    }
    try {
        const nlohmann::json document = nlohmann::json::parse(json.begin(), json.end());
        if (!document.is_object()) throw std::invalid_argument("the layout must be a JSON object");
        const auto schema = document.find("schemaVersion");
        if (schema != document.end() && schema->is_number_integer() &&
            schema->get<std::int64_t>() != static_cast<std::int64_t>(kPlatformUiLayoutSchemaVersion)) {
            throw std::invalid_argument("unsupported schemaVersion");
        }
        PlatformUiLayoutDocument layout;
        layout.schema_version = kPlatformUiLayoutSchemaVersion;
        if (const auto canvas = document.find("canvas");
            canvas != document.end() && canvas->is_object()) {
            layout.canvas_width = ClampFinite(
                ReadFloat(*canvas, "width", kPlatformUiLayoutDefaultCanvasWidth),
                1.0f, 32768.0f, kPlatformUiLayoutDefaultCanvasWidth);
            layout.canvas_height = ClampFinite(
                ReadFloat(*canvas, "height", kPlatformUiLayoutDefaultCanvasHeight),
                1.0f, 32768.0f, kPlatformUiLayoutDefaultCanvasHeight);
        } else {
            layout.canvas_width = kPlatformUiLayoutDefaultCanvasWidth;
            layout.canvas_height = kPlatformUiLayoutDefaultCanvasHeight;
        }
        layout.surface_opacity = ClampFinite(
            ReadFloat(document, "surfaceOpacity", 1.0f),
            kPlatformUiLayoutSurfaceOpacityMinimum, 1.0f, 1.0f);
        layout.background = ParseBackground(document);
        layout.stickers = ParseStickers(document);
        result.ok = true;
        result.layout = std::move(layout);
        return result;
    } catch (const std::exception& error) {
        result.message = error.what();
        return result;
    } catch (...) {
        result.message = "the layout could not be parsed";
        return result;
    }
}

bool ResolvePlatformUiLayoutPath(
    const std::filesystem::path& runtime_root, const std::string_view relative_path,
    std::filesystem::path& resolved) noexcept {
    try {
        if (relative_path.empty() || relative_path.size() > kPlatformUiLayoutMaximumPathLength) {
            return false;
        }
        const std::filesystem::path candidate(Utf8ToWide(relative_path));
        if (candidate.is_absolute() || candidate.has_root_name() ||
            candidate.has_root_directory()) {
            return false;
        }
        for (const std::filesystem::path& part : candidate) {
            if (part == L".." || part == L".") return false;
        }
        resolved = runtime_root / candidate;
        return true;
    } catch (...) {
        return false;
    }
}

PlatformUiImagePlacement ComputePlatformUiBackgroundPlacement(
    const PlatformUiBackgroundMode mode, const float image_width, const float image_height,
    const float viewport_x, const float viewport_y, const float viewport_width,
    const float viewport_height) noexcept {
    PlatformUiImagePlacement placement;
    if (mode == PlatformUiBackgroundMode::Disabled) return placement;
    if (!IsUsableDimension(image_width) || !IsUsableDimension(image_height) ||
        !IsUsableDimension(viewport_width) || !IsUsableDimension(viewport_height)) {
        return placement;
    }
    switch (mode) {
    case PlatformUiBackgroundMode::Center: {
        const float scale =
            (std::min)(viewport_width / image_width, viewport_height / image_height);
        placement.width = image_width * scale;
        placement.height = image_height * scale;
        placement.x = viewport_x + (viewport_width - placement.width) * 0.5f;
        placement.y = viewport_y + (viewport_height - placement.height) * 0.5f;
        break;
    }
    case PlatformUiBackgroundMode::Crop: {
        const float scale =
            (std::max)(viewport_width / image_width, viewport_height / image_height);
        placement.x = viewport_x;
        placement.y = viewport_y;
        placement.width = viewport_width;
        placement.height = viewport_height;
        const float visible_width = viewport_width / (image_width * scale);
        const float visible_height = viewport_height / (image_height * scale);
        placement.uv0_x = 0.5f - visible_width * 0.5f;
        placement.uv0_y = 0.5f - visible_height * 0.5f;
        placement.uv1_x = 0.5f + visible_width * 0.5f;
        placement.uv1_y = 0.5f + visible_height * 0.5f;
        break;
    }
    case PlatformUiBackgroundMode::Stretch:
        placement.x = viewport_x;
        placement.y = viewport_y;
        placement.width = viewport_width;
        placement.height = viewport_height;
        break;
    case PlatformUiBackgroundMode::Disabled:
        break;
    }
    return placement;
}

PlatformUiStickerPlacement ComputePlatformUiStickerPlacement(
    const PlatformUiSticker& sticker, const float image_width, const float image_height,
    const float viewport_width, const float viewport_height) noexcept {
    PlatformUiStickerPlacement placement;
    if (!IsUsableDimension(viewport_width) || !IsUsableDimension(viewport_height)) {
        return placement;
    }
    const bool has_image = IsUsableDimension(image_width) && IsUsableDimension(image_height);
    float width = 0.0f;
    float height = 0.0f;
    if (sticker.width > 0.0f && sticker.height > 0.0f) {
        width = sticker.width * viewport_width;
        height = sticker.height * viewport_height;
    } else if (sticker.width > 0.0f) {
        width = sticker.width * viewport_width;
        height = has_image ? width * image_height / image_width : width;
    } else if (sticker.height > 0.0f) {
        height = sticker.height * viewport_height;
        width = has_image ? height * image_width / image_height : height;
    } else if (has_image) {
        width = image_width;
        height = image_height;
    }
    if (!IsUsableDimension(width) || !IsUsableDimension(height)) return placement;
    const float anchor_x = sticker.x * viewport_width;
    const float anchor_y = sticker.y * viewport_height;
    switch (sticker.anchor) {
    case PlatformUiAnchor::Center:
        placement.x = anchor_x - width * 0.5f;
        placement.y = anchor_y - height * 0.5f;
        break;
    case PlatformUiAnchor::TopLeft:
        placement.x = anchor_x;
        placement.y = anchor_y;
        break;
    case PlatformUiAnchor::TopRight:
        placement.x = anchor_x - width;
        placement.y = anchor_y;
        break;
    case PlatformUiAnchor::BottomLeft:
        placement.x = anchor_x;
        placement.y = anchor_y - height;
        break;
    case PlatformUiAnchor::BottomRight:
        placement.x = anchor_x - width;
        placement.y = anchor_y - height;
        break;
    }
    placement.width = width;
    placement.height = height;
    placement.rotation = std::isfinite(sticker.rotation) ? sticker.rotation : 0.0f;
    placement.uv0_x = sticker.flip_x ? 1.0f : 0.0f;
    placement.uv0_y = sticker.flip_y ? 1.0f : 0.0f;
    placement.uv1_x = sticker.flip_x ? 0.0f : 1.0f;
    placement.uv1_y = sticker.flip_y ? 0.0f : 1.0f;
    placement.alpha = sticker.opacity;
    return placement;
}

PlatformUiImageQuad ComputePlatformUiImageQuad(
    const PlatformUiStickerPlacement& placement) noexcept {
    // Corners run clockwise from the top-left in screen space, where y grows
    // downwards, so a positive angle turns the sticker clockwise on screen.
    constexpr float kCornerX[4]{-1.0f, 1.0f, 1.0f, -1.0f};
    constexpr float kCornerY[4]{-1.0f, -1.0f, 1.0f, 1.0f};
    constexpr float kCornerU[4]{0.0f, 1.0f, 1.0f, 0.0f};
    constexpr float kCornerV[4]{0.0f, 0.0f, 1.0f, 1.0f};
    PlatformUiImageQuad quad;
    const float centre_x = placement.x + placement.width * 0.5f;
    const float centre_y = placement.y + placement.height * 0.5f;
    const float half_width = placement.width * 0.5f;
    const float half_height = placement.height * 0.5f;
    const float radians = placement.rotation * kDegreesToRadians;
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    for (int index = 0; index < 4; ++index) {
        const float local_x = kCornerX[index] * half_width;
        const float local_y = kCornerY[index] * half_height;
        quad.x[index] = centre_x + local_x * cosine - local_y * sine;
        quad.y[index] = centre_y + local_x * sine + local_y * cosine;
        quad.u[index] = placement.uv0_x + (placement.uv1_x - placement.uv0_x) * kCornerU[index];
        quad.v[index] = placement.uv0_y + (placement.uv1_y - placement.uv0_y) * kCornerV[index];
    }
    return quad;
}

std::filesystem::path ResolvePlatformUiLayoutFile(
    const std::filesystem::path& runtime_root, const std::string_view palette) noexcept {
    const std::filesystem::path directory =
        runtime_root / std::filesystem::path(kPlatformUiLayoutDirectory);
    const std::filesystem::path shared =
        directory / std::filesystem::path(kPlatformUiLayoutSharedFileName);
    if (palette.empty()) return shared;
    std::filesystem::path themed = directory / std::filesystem::path(palette);
    themed += L".json";
    std::error_code error;
    const bool present = std::filesystem::is_regular_file(themed, error);
    return present && !error ? themed : shared;
}

namespace {

// nlohmann rejects any string that is not valid UTF-8, and a throw inside this
// translation unit reaches std::terminate. Paths arrive from the file picker in
// the local ANSI code page, so text that is not already valid UTF-8 is folded
// down to ASCII before it enters the document.
bool IsValidUtf8(const std::string& text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 0;
        if (lead < 0x80) {
            length = 1;
        } else if ((lead & 0xe0) == 0xc0) {
            length = 2;
        } else if ((lead & 0xf0) == 0xe0) {
            length = 3;
        } else if ((lead & 0xf8) == 0xf0) {
            length = 4;
        } else {
            return false;
        }
        if (index + length > text.size()) return false;
        for (std::size_t offset = 1; offset < length; ++offset) {
            if ((static_cast<unsigned char>(text[index + offset]) & 0xc0) != 0x80) {
                return false;
            }
        }
        index += length;
    }
    return true;
}

std::string ToSerializableText(const std::string& text) {
    if (IsValidUtf8(text)) return text;
    std::string folded;
    folded.reserve(text.size());
    for (const unsigned char character : text) {
        folded.push_back(character < 0x80 ? static_cast<char>(character) : '?');
    }
    return folded;
}

}  // namespace
std::string SerializePlatformUiLayout(const PlatformUiLayoutDocument& layout) {
    const auto color_text = [](const PlatformUiColor& color) {
        const auto channel = [](const float value) {
            const float clamped = std::clamp(value, 0.0f, 1.0f);
            return static_cast<int>(clamped * 255.0f + 0.5f);
        };
        char buffer[8]{};
        std::snprintf(
            buffer, sizeof(buffer), "#%02x%02x%02x", channel(color.red),
            channel(color.green), channel(color.blue));
        return std::string(buffer);
    };

    nlohmann::json document = nlohmann::json::object();
    document["schemaVersion"] = kPlatformUiLayoutSchemaVersion;
    document["canvas"] = {
        {"width", layout.canvas_width}, {"height", layout.canvas_height}};
    document["surfaceOpacity"] = layout.surface_opacity;
    // The background is optional: a layout that only carries stickers is a valid
    // theme, and writing an empty placeholder path would misrepresent it.
    if (!layout.background.path.empty() ||
        layout.background.mode != PlatformUiBackgroundMode::Disabled) {
        document["background"] = {
            {"path", ToSerializableText(layout.background.path)},
            {"mode", std::string(ToString(layout.background.mode))},
            {"opacity", layout.background.opacity},
            {"tint", color_text(layout.background.tint)}};
    }
    document["stickers"] = nlohmann::json::array();
    for (const PlatformUiSticker& sticker : layout.stickers) {
        document["stickers"].push_back(
            {{"path", ToSerializableText(sticker.path)},
             {"x", sticker.x},
             {"y", sticker.y},
             {"width", sticker.width},
             {"height", sticker.height},
             {"rotation", sticker.rotation},
             {"opacity", sticker.opacity},
             {"anchor", std::string(ToString(sticker.anchor))},
             {"layer", std::string(ToString(sticker.layer))},
             {"tint", color_text(sticker.tint)},
             {"flipX", sticker.flip_x},
             {"flipY", sticker.flip_y}});
    }
    // Layout text comes from files and paths the user may have authored in any
    // local encoding. Dumping with the strict handler would raise
    // type_error.316 and, because this runs under a noexcept boundary, turn a
    // stray byte into std::terminate.
    return document.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

bool WritePlatformUiLayout(
    const std::filesystem::path& path, const PlatformUiLayoutDocument& layout,
    std::string& error) noexcept {
    error.clear();
    try {
        const std::string text = SerializePlatformUiLayout(layout);
        // Written beside the target and then renamed, so the worker's stamp poll
        // never observes a partially written document.
        std::filesystem::path temporary = path;
        temporary += L".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) {
                error = "cannot open " + temporary.string();
                return false;
            }
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!stream) {
                error = "cannot write " + temporary.string();
                return false;
            }
        }
        std::error_code code;
        std::filesystem::rename(temporary, path, code);
        if (code) {
            std::filesystem::remove(temporary, code);
            error = "cannot replace " + path.string();
            return false;
        }
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    } catch (...) {
        error = "unknown failure";
        return false;
    }
}

}  // namespace anomaly
