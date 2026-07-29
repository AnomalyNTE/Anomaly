#pragma once

#include "anomaly/ui_resource_registry.hpp"

#include <cstdint>
#include <memory>

namespace anomaly {

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
