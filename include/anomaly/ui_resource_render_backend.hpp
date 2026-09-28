#pragma once

#include "anomaly/ui_resource_registry.hpp"

#include <cstdint>
#include <memory>

namespace anomaly {

// A themed background or sticker draw. The rectangle is in absolute screen
// space; the source rectangle is normalized image space, and rotation is
// clockwise in degrees around the rectangle centre. A flipped source rectangle
// (uv1 below uv0) is valid and must be honoured.
struct UiTextureDrawRequest final {
    float x{};
    float y{};
    float width{};
    float height{};
    float rotation_degrees{};
    float uv0_x{};
    float uv0_y{};
    float uv1_x{1.0f};
    float uv1_y{1.0f};
    std::uint32_t tint_rgba{0xffffffffU};
    // Draw above every ImGui window instead of into the current one. The caller
    // is responsible for clipping the overlay to its own region.
    bool on_top{};
    // Draw behind every ImGui window. Stickers use this so they stay under the
    // shell's labels while still being free to overhang the shell window, which
    // the window's own clip rectangle would otherwise cut off.
    bool behind{};
};

// Render-owned bridge for the logical resource registry. Implementations may
// use ImGui and D3D12 internally, but neither type crosses this boundary.
class UiResourceRenderBackend {
public:
    virtual ~UiResourceRenderBackend() = default;

    virtual bool PushFont(
        UiResourceRegistry& registry, const std::shared_ptr<PluginScope>& scope,
        UiResourceHandle handle) noexcept = 0;
    virtual bool PopFont() noexcept = 0;
    virtual bool DrawTexture(
        UiResourceRegistry& registry, const std::shared_ptr<PluginScope>& scope,
        UiResourceHandle handle, float width, float height, std::uint32_t tint_rgba) noexcept = 0;
    // Draws an already-ready texture at an explicit screen rectangle. Used by
    // the host theme layout, which positions images against the shell viewport
    // instead of the current ImGui cursor.
    virtual bool DrawTextureEx(
        UiResourceRegistry& registry, const std::shared_ptr<PluginScope>& scope,
        UiResourceHandle handle, const UiTextureDrawRequest& request) noexcept = 0;

    // Called before a plugin Draw callback. Font atlas work has to happen
    // before ImGui locks the frame; queued requests without Worker-staged
    // bytes remain queued rather than becoming spuriously ready.
    virtual void PrepareFont(
        UiResourceRegistry& registry, const std::shared_ptr<PluginScope>& scope,
        UiResourceHandle handle) noexcept = 0;

    // Queued textures have already been decoded to RGBA8 by a Worker. The
    // render bridge records the bounded GPU upload before plugin Draw starts,
    // which lets plugins observe a Ready state without having to issue a
    // speculative draw first.
    virtual void PrepareTexture(
        UiResourceRegistry& registry, const std::shared_ptr<PluginScope>& scope,
        UiResourceHandle handle) noexcept = 0;

    // Called once per render frame to release backend objects whose scope
    // lease was revoked without another draw call.
    virtual void CollectGarbage(UiResourceRegistry& registry) noexcept = 0;
    virtual void OnDeviceLost() noexcept = 0;
    virtual bool OnDeviceRebuilt(std::uint64_t device_generation) noexcept = 0;
};

}  // namespace anomaly
