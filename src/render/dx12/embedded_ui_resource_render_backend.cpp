#include "embedded_ui_resource_render_backend.hpp"

#include "embedded_host_internal.hpp"

#include "anomaly/platform_ui_theme.hpp"
#include "anomaly/ui_resource_render_backend.hpp"

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_dx12.h>

#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ue5mem::embedded {
namespace {

constexpr std::uint64_t kUiTextureCacheBudgetBytes = 128ULL * 1024ULL * 1024ULL;

[[nodiscard]] bool IsUsableDimension(const float value) noexcept {
    return std::isfinite(value) && value > 0.0F;
}

// Texture cache admission accounts for the logical RGBA texture and the
// temporary row-pitch-padded upload resource. The latter is returned once its
// copy submission fence completes. D3D11 has no such staging resource: the
// pixels are handed to CreateTexture2D directly, so it accounts for nothing.
[[nodiscard]] bool EstimateTextureUploadBytes(
    const anomaly::UiTextureRequest& request, std::uint64_t* upload_bytes,
    const bool immediate_upload) noexcept {
    if (upload_bytes == nullptr || request.width == 0 || request.height == 0) return false;
    if (immediate_upload) {
        *upload_bytes = 0;
        return true;
    }
    constexpr std::uint64_t pitch_alignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    constexpr std::uint64_t placement_alignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    const std::uint64_t row_bytes = static_cast<std::uint64_t>(request.width) * 4U;
    if (row_bytes == 0 || row_bytes > (std::numeric_limits<std::uint64_t>::max)() -
            (pitch_alignment - 1U)) {
        return false;
    }
    const std::uint64_t row_pitch =
        ((row_bytes + pitch_alignment - 1U) / pitch_alignment) * pitch_alignment;
    if (request.height > (std::numeric_limits<std::uint64_t>::max)() / row_pitch) return false;
    const std::uint64_t aligned_rows = row_pitch * request.height;
    if (aligned_rows > (std::numeric_limits<std::uint64_t>::max)() -
            (placement_alignment - 1U)) {
        return false;
    }
    *upload_bytes = ((aligned_rows + placement_alignment - 1U) / placement_alignment) *
        placement_alignment;
    return *upload_bytes != 0;
}

[[nodiscard]] const ImWchar* GlyphRangeFor(
    ImFontAtlas& atlas, const anomaly::UiGlyphRange range) noexcept {
    switch (range) {
    case anomaly::UiGlyphRange::Cyrillic:
        return atlas.GetGlyphRangesCyrillic();
    case anomaly::UiGlyphRange::Japanese:
        return atlas.GetGlyphRangesJapanese();
    case anomaly::UiGlyphRange::ChineseFull:
        return atlas.GetGlyphRangesChineseFull();
    case anomaly::UiGlyphRange::Default:
    case anomaly::UiGlyphRange::Latin:
    default:
        return atlas.GetGlyphRangesDefault();
    }
}

struct LeaseReference final {
    std::weak_ptr<anomaly::PluginScope> scope;
    anomaly::UiResourceHandle handle;
};

struct TextureEntry final {
    std::vector<LeaseReference> leases;
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    // D3D11 keeps the texture and its view instead; the view pointer is what
    // ImGui draws with, standing in for the D3D12 descriptor below.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d11_texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> d3d11_view;

    // Whether this entry already holds something drawable. Which member proves
    // that differs by API, and testing the wrong one silently turns the
    // "already uploaded" check into a miss, re-uploading every single frame.
    [[nodiscard]] bool Resident(const bool immediate) const noexcept {
        return immediate ? d3d11_view != nullptr : gpu.ptr != 0;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    std::uint64_t byte_size{};
    std::uint64_t upload_byte_size{};
    std::uint64_t release_fence{};
    std::uint64_t upload_fence{};
    std::uint64_t device_generation{};
    std::uint32_t width{};
    std::uint32_t height{};
};

struct FontEntry final {
    std::vector<LeaseReference> leases;
    std::vector<std::uint8_t> bytes;
    float size_pixels{};
    anomaly::UiGlyphRange glyph_range{anomaly::UiGlyphRange::Default};
    ImFont* font{};
    bool failed{};
};

class EmbeddedUiResourceRenderBackend final : public anomaly::UiResourceRenderBackend {
public:
    explicit EmbeddedUiResourceRenderBackend(EmbeddedState& state) noexcept : state_(state) {}

    ~EmbeddedUiResourceRenderBackend() override { ReleaseTextures(true); }

    bool PushFont(
        anomaly::UiResourceRegistry& registry,
        const std::shared_ptr<anomaly::PluginScope>& scope,
        const anomaly::UiResourceHandle handle) noexcept override {
        try {
            if (scope == nullptr || !handle || ImGui::GetCurrentContext() == nullptr) return false;
            const auto resource = registry.ResourceState(scope, handle);
            if (!resource || resource->kind != anomaly::UiResourceKind::Font ||
                resource->resource_id == 0 || resource->state != anomaly::UiResourceState::Ready ||
                resource->device_generation != device_generation_) {
                return false;
            }
            const auto found = fonts_.find(resource->resource_id);
            if (found == fonts_.end() || found->second.font == nullptr || found->second.failed) {
                return false;
            }
            TrackLease(found->second.leases, scope, handle);
            ImGui::PushFont(found->second.font);
            ++pushed_fonts_;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool PopFont() noexcept override {
        try {
            if (pushed_fonts_ == 0 || ImGui::GetCurrentContext() == nullptr) return false;
            ImGui::PopFont();
            --pushed_fonts_;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool DrawTexture(
        anomaly::UiResourceRegistry& registry,
        const std::shared_ptr<anomaly::PluginScope>& scope,
        const anomaly::UiResourceHandle handle,
        const float width,
        const float height,
        const std::uint32_t tint_rgba) noexcept override {
        try {
            if (scope == nullptr || !handle || ImGui::GetCurrentContext() == nullptr) return false;
            const auto resource = registry.ResourceState(scope, handle);
            if (!resource || resource->kind != anomaly::UiResourceKind::Texture ||
                resource->resource_id == 0 || resource->state != anomaly::UiResourceState::Ready ||
                resource->device_generation != device_generation_) {
                return false;
            }
            const auto found = textures_.find(resource->resource_id);
            const ImTextureID texture_id = ImmediateMode()
                ? reinterpret_cast<ImTextureID>(found == textures_.end()
                      ? nullptr : found->second.d3d11_view.Get())
                : static_cast<ImTextureID>(
                      found == textures_.end() ? 0 : found->second.gpu.ptr);
            if (found == textures_.end() || texture_id == ImTextureID{} ||
                found->second.device_generation != device_generation_) {
                return false;
            }
            TrackLease(found->second.leases, scope, handle);
            found->second.release_fence =
                (std::max)(found->second.release_fence, state_.next_fence_value);
            const float resolved_width = IsUsableDimension(width)
                ? width : static_cast<float>(found->second.width);
            const float resolved_height = IsUsableDimension(height)
                ? height : static_cast<float>(found->second.height);
            if (!IsUsableDimension(resolved_width) || !IsUsableDimension(resolved_height)) return false;
            const ImVec4 tint(
                static_cast<float>(tint_rgba & 0xffU) / 255.0F,
                static_cast<float>((tint_rgba >> 8U) & 0xffU) / 255.0F,
                static_cast<float>((tint_rgba >> 16U) & 0xffU) / 255.0F,
                static_cast<float>((tint_rgba >> 24U) & 0xffU) / 255.0F);
            ImGui::ImageWithBg(
                texture_id,
                ImVec2(resolved_width, resolved_height), ImVec2(0.0F, 0.0F),
                ImVec2(1.0F, 1.0F), ImVec4(0.0F, 0.0F, 0.0F, 0.0F), tint);
            return true;
        } catch (...) {
            return false;
        }
    }

    void PrepareFont(
        anomaly::UiResourceRegistry& registry,
        const std::shared_ptr<anomaly::PluginScope>& scope,
        const anomaly::UiResourceHandle handle) noexcept override {
        try {
            if (scope == nullptr || !handle || !CanTouchImGui()) return;
            const auto resource = registry.ResourceState(scope, handle);
            if (!resource || resource->kind != anomaly::UiResourceKind::Font ||
                resource->resource_id == 0) {
                return;
            }
            const std::uint64_t resource_id = resource->resource_id;
            const auto found = fonts_.find(resource_id);
            if (found != fonts_.end()) TrackLease(found->second.leases, scope, handle);
            if (resource->state == anomaly::UiResourceState::Failed) {
                if (found != fonts_.end() && !found->second.failed) {
                    found->second.failed = true;
                    found->second.font = nullptr;
                    font_atlas_dirty_ = true;
                }
                return;
            }
            if (resource->state == anomaly::UiResourceState::Ready &&
                resource->device_generation == device_generation_ && found != fonts_.end() &&
                found->second.font != nullptr && !found->second.failed) {
                return;
            }
            if (resource->state != anomaly::UiResourceState::Queued &&
                resource->state != anomaly::UiResourceState::StaleDevice &&
                resource->state != anomaly::UiResourceState::Ready) {
                return;
            }
            // Copy the payload only for a new, stale, or missing atlas entry.
            const auto state = registry.FontState(scope, handle);
            if (!state || state->resource.resource_id != resource_id ||
                state->resource.state == anomaly::UiResourceState::Failed) {
                return;
            }
            const anomaly::UiFontRequest& request = state->request;
            if (request.encoded_bytes.empty()) return;

            if (found == fonts_.end()) {
                FontEntry entry;
                TrackLease(entry.leases, scope, handle);
                entry.bytes = request.encoded_bytes;
                entry.size_pixels = state->effective_size_pixels;
                entry.glyph_range = request.glyph_range;
                fonts_.emplace(resource_id, std::move(entry));
                font_atlas_dirty_ = true;
            } else if (found->second.bytes != request.encoded_bytes ||
                       found->second.size_pixels != state->effective_size_pixels ||
                       found->second.glyph_range != request.glyph_range ||
                       state->resource.state != anomaly::UiResourceState::Ready ||
                       found->second.failed) {
                found->second.bytes = request.encoded_bytes;
                found->second.size_pixels = state->effective_size_pixels;
                found->second.glyph_range = request.glyph_range;
                found->second.font = nullptr;
                found->second.failed = false;
                font_atlas_dirty_ = true;
            }
            RebuildFontAtlas(registry);
        } catch (...) {
            static_cast<void>(registry.MarkFontFailed(scope, handle));
        }
    }

    void PrepareTexture(
        anomaly::UiResourceRegistry& registry,
        const std::shared_ptr<anomaly::PluginScope>& scope,
        const anomaly::UiResourceHandle handle) noexcept override {
        try {
            if (scope == nullptr || !handle || !CanUploadTexture()) {
                return;
            }
            const auto resource = registry.ResourceState(scope, handle);
            if (!resource || resource->kind != anomaly::UiResourceKind::Texture ||
                resource->resource_id == 0) {
                return;
            }
            const std::uint64_t resource_id = resource->resource_id;
            const auto existing = textures_.find(resource_id);
            if (existing != textures_.end()) TrackLease(existing->second.leases, scope, handle);
            if (resource->state == anomaly::UiResourceState::Failed) {
                return;
            }

            const std::uint64_t generation = registry.DeviceGeneration();
            if (resource->state == anomaly::UiResourceState::Ready &&
                resource->device_generation == generation && existing != textures_.end() &&
                existing->second.device_generation == generation &&
                existing->second.Resident(ImmediateMode())) {
                return;
            }
            if (resource->state != anomaly::UiResourceState::Queued &&
                resource->state != anomaly::UiResourceState::StaleDevice &&
                resource->state != anomaly::UiResourceState::Ready) {
                return;
            }
            // Copy the payload only when the cache must upload or recover it.
            const auto state = registry.TextureState(scope, handle);
            if (!state || state->resource.resource_id != resource_id ||
                state->resource.state == anomaly::UiResourceState::Failed ||
                state->request.format != anomaly::UiTextureFormat::Rgba8 ||
                state->request.encoded_bytes.empty()) {
                return;
            }
            const std::uint64_t byte_size = state->request.encoded_bytes.size();
            std::uint64_t maximum_upload_bytes{};
            if (!EstimateTextureUploadBytes(
                    state->request, &maximum_upload_bytes, ImmediateMode())) {
                static_cast<void>(registry.MarkTextureFailed(scope, handle));
                return;
            }

            std::vector<LeaseReference> leases;
            if (existing != textures_.end()) {
                leases = std::move(existing->second.leases);
                RetireTexture(resource_id);
            }
            ReclaimCompletedUploads();
            const auto [entry_iterator, inserted] = textures_.try_emplace(resource_id);
            static_cast<void>(inserted);
            TextureEntry& entry = entry_iterator->second;
            entry.leases = std::move(leases);
            TrackLease(entry.leases, scope, handle);
            if (byte_size == 0 || byte_size > kUiTextureCacheBudgetBytes ||
                maximum_upload_bytes > kUiTextureCacheBudgetBytes - byte_size ||
                resident_texture_bytes_ >
                    kUiTextureCacheBudgetBytes - byte_size - maximum_upload_bytes) {
                MarkTextureFailedForLeases(registry, resource_id, entry);
                textures_.erase(entry_iterator);
                return;
            }

            entry.device_generation = generation;
            entry.width = state->request.width;
            entry.height = state->request.height;
            if (!CreateTexture(entry, state->request, maximum_upload_bytes)) {
                ReleaseTexture(entry);
                MarkTextureFailedForLeases(registry, resource_id, entry);
                textures_.erase(entry_iterator);
                return;
            }
            entry.byte_size = byte_size;
            resident_texture_bytes_ += byte_size + entry.upload_byte_size;
            if (!MarkTextureReadyForLeases(
                    registry, resource_id, entry, generation,
                    state->request.width, state->request.height)) {
                RetireTexture(resource_id);
            }
        } catch (...) {
            static_cast<void>(registry.MarkTextureFailed(scope, handle));
        }
    }

    void CollectGarbage(anomaly::UiResourceRegistry& registry) noexcept override {
        try {
            ReclaimCompletedUploads();
            for (auto iterator = textures_.begin(); iterator != textures_.end();) {
                const std::uint64_t resource_id = iterator->first;
                if (!PruneTextureLeases(registry, resource_id, iterator->second) &&
                    !registry.IsResourceLive(resource_id)) {
                    ++iterator;
                    RetireTexture(resource_id);
                } else {
                    ++iterator;
                }
            }
            for (auto iterator = fonts_.begin(); iterator != fonts_.end();) {
                const std::uint64_t resource_id = iterator->first;
                if (!PruneFontLeases(registry, resource_id, iterator->second) &&
                    !registry.IsResourceLive(resource_id)) {
                    retired_fonts_.push_back(std::move(iterator->second));
                    iterator = fonts_.erase(iterator);
                    font_atlas_dirty_ = true;
                } else {
                    ++iterator;
                }
            }
            ReclaimRetiredTextures();
            RebuildFontAtlas(registry);
        } catch (...) {
        }
    }

    void OnDeviceLost() noexcept override {
        try {
            pushed_fonts_ = 0;
            device_generation_ = 0;
            font_atlas_dirty_ = true;
            ReleaseTextures(true);
        } catch (...) {
        }
    }

    bool OnDeviceRebuilt(const std::uint64_t device_generation) noexcept override {
        if (device_generation == 0 || !CanTouchImGui()) return false;
        device_generation_ = device_generation;
        font_atlas_dirty_ = true;
        return true;
    }

private:
    // True while the overlay draws through the immediate context, which is the
    // whole of D3D11 submission: no queue, no fences, no descriptor heaps, and
    // therefore none of the deferral the D3D12 paths below need.
    [[nodiscard]] bool ImmediateMode() const noexcept {
        return state_.render_api == EmbeddedRenderApi::D3D11;
    }

    [[nodiscard]] bool CanTouchImGui() const noexcept {
        if (state_.imgui_context == nullptr ||
            ImGui::GetCurrentContext() != state_.imgui_context) {
            return false;
        }
        if (ImmediateMode()) {
            return state_.dx11_initialized && state_.d3d11_device != nullptr;
        }
        return state_.dx12_initialized && state_.device != nullptr &&
            state_.shader_heap != nullptr;
    }

    [[nodiscard]] bool CanUploadTexture() const noexcept {
        if (!CanTouchImGui()) return false;
        if (ImmediateMode()) return true;
        return state_.command_list != nullptr && state_.shader_descriptors.Available() != 0;
    }

    [[nodiscard]] bool CanRebuildFontAtlas() const noexcept {
        if (!CanTouchImGui()) return false;
        // Nothing is in flight to wait for when submission is immediate.
        if (ImmediateMode()) return true;
        if (state_.fence == nullptr || FAILED(state_.device->GetDeviceRemovedReason())) {
            return false;
        }
        const std::uint64_t completed = state_.fence->GetCompletedValue();
        for (const FrameContext& frame : state_.frames) {
            if (frame.fence_value != 0 && completed < frame.fence_value) return false;
        }
        return true;
    }

    static void TrackLease(
        std::vector<LeaseReference>& leases,
        const std::shared_ptr<anomaly::PluginScope>& scope,
        const anomaly::UiResourceHandle handle) {
        const auto found = std::find_if(
            leases.begin(), leases.end(), [handle](const LeaseReference& lease) {
                return lease.handle == handle;
            });
        if (found != leases.end()) {
            found->scope = scope;
            return;
        }
        leases.push_back({scope, handle});
    }

    template <typename IsLive>
    [[nodiscard]] static bool PruneLeases(
        std::vector<LeaseReference>& leases, IsLive&& is_live) {
        const auto stale = std::remove_if(
            leases.begin(), leases.end(), [&is_live](const LeaseReference& lease) {
                return !is_live(lease);
            });
        leases.erase(stale, leases.end());
        return !leases.empty();
    }

    [[nodiscard]] bool PruneFontLeases(
        anomaly::UiResourceRegistry& registry,
        const std::uint64_t resource_id,
        FontEntry& entry) {
        return PruneLeases(entry.leases, [&registry, resource_id](const LeaseReference& lease) {
            const auto scope = lease.scope.lock();
            if (scope == nullptr) return false;
            const auto resource = registry.ResourceState(scope, lease.handle);
            return resource && resource->kind == anomaly::UiResourceKind::Font &&
                resource->resource_id == resource_id;
        });
    }

    [[nodiscard]] bool PruneTextureLeases(
        anomaly::UiResourceRegistry& registry,
        const std::uint64_t resource_id,
        TextureEntry& entry) {
        return PruneLeases(entry.leases, [&registry, resource_id](const LeaseReference& lease) {
            const auto scope = lease.scope.lock();
            if (scope == nullptr) return false;
            const auto resource = registry.ResourceState(scope, lease.handle);
            return resource && resource->kind == anomaly::UiResourceKind::Texture &&
                resource->resource_id == resource_id;
        });
    }

    void MarkFontFailedForLeases(
        anomaly::UiResourceRegistry& registry,
        const std::uint64_t resource_id,
        FontEntry& entry) {
        static_cast<void>(PruneFontLeases(registry, resource_id, entry));
        for (const LeaseReference& lease : entry.leases) {
            const auto scope = lease.scope.lock();
            if (scope != nullptr) {
                static_cast<void>(registry.MarkFontFailed(scope, lease.handle));
            }
        }
    }

    void MarkFontReadyForLeases(
        anomaly::UiResourceRegistry& registry,
        const std::uint64_t resource_id,
        FontEntry& entry,
        const std::uint64_t generation) {
        static_cast<void>(PruneFontLeases(registry, resource_id, entry));
        for (const LeaseReference& lease : entry.leases) {
            const auto scope = lease.scope.lock();
            if (scope != nullptr) {
                static_cast<void>(registry.MarkFontReady(scope, lease.handle, generation));
            }
        }
    }

    void MarkTextureFailedForLeases(
        anomaly::UiResourceRegistry& registry,
        const std::uint64_t resource_id,
        TextureEntry& entry) {
        static_cast<void>(PruneTextureLeases(registry, resource_id, entry));
        for (const LeaseReference& lease : entry.leases) {
            const auto scope = lease.scope.lock();
            if (scope != nullptr) {
                static_cast<void>(registry.MarkTextureFailed(scope, lease.handle));
            }
        }
    }

    [[nodiscard]] bool MarkTextureReadyForLeases(
        anomaly::UiResourceRegistry& registry,
        const std::uint64_t resource_id,
        TextureEntry& entry,
        const std::uint64_t generation,
        const std::uint32_t width,
        const std::uint32_t height) {
        static_cast<void>(PruneTextureLeases(registry, resource_id, entry));
        bool marked{};
        for (const LeaseReference& lease : entry.leases) {
            const auto scope = lease.scope.lock();
            if (scope != nullptr) {
                marked = registry.MarkTextureReady(
                    scope, lease.handle, generation, width, height) || marked;
            }
        }
        return marked;
    }

    // Creates an immutable texture directly from the decoded pixels. There is
    // no staging resource to account for, no copy to record and no fence to
    // wait on before the view can be drawn with.
    [[nodiscard]] bool CreateTextureD3D11(
        TextureEntry& entry, const anomaly::UiTextureRequest& request) noexcept {
        if (state_.d3d11_device == nullptr) return false;
        D3D11_TEXTURE2D_DESC description{};
        description.Width = request.width;
        description.Height = request.height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = request.encoded_bytes.data();
        data.SysMemPitch = request.width * 4U;
        if (FAILED(state_.d3d11_device->CreateTexture2D(
                &description, &data, entry.d3d11_texture.ReleaseAndGetAddressOf()))) {
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        if (FAILED(state_.d3d11_device->CreateShaderResourceView(
                entry.d3d11_texture.Get(), &view,
                entry.d3d11_view.ReleaseAndGetAddressOf()))) {
            entry.d3d11_texture.Reset();
            return false;
        }
        entry.upload_byte_size = 0;
        entry.release_fence = 0;
        entry.upload_fence = 0;
        return true;
    }

    [[nodiscard]] bool CreateTexture(
        TextureEntry& entry, const anomaly::UiTextureRequest& request,
        const std::uint64_t maximum_upload_bytes) noexcept {
        if (request.width == 0 || request.height == 0 || request.encoded_bytes.empty() ||
            static_cast<std::uint64_t>(request.width) * request.height * 4U !=
                request.encoded_bytes.size()) {
            return false;
        }
        if (ImmediateMode()) return CreateTextureD3D11(entry, request);
        if (state_.device == nullptr || state_.command_list == nullptr) return false;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
        if (!state_.shader_descriptors.Allocate(&cpu, &gpu)) return false;
        entry.cpu = cpu;
        entry.gpu = gpu;

        D3D12_HEAP_PROPERTIES default_heap{};
        default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC texture{};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width = request.width;
        texture.Height = request.height;
        texture.DepthOrArraySize = 1;
        texture.MipLevels = 1;
        texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture.SampleDesc.Count = 1;
        texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (FAILED(state_.device->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &texture,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(entry.texture.ReleaseAndGetAddressOf())))) {
            return false;
        }

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows{};
        UINT64 row_bytes{};
        UINT64 upload_bytes{};
        state_.device->GetCopyableFootprints(
            &texture, 0, 1, 0, &footprint, &rows, &row_bytes, &upload_bytes);
        const std::uint64_t source_row_bytes = static_cast<std::uint64_t>(request.width) * 4U;
        if (rows != request.height || row_bytes != source_row_bytes ||
            footprint.Footprint.RowPitch < source_row_bytes || upload_bytes == 0 ||
            upload_bytes > maximum_upload_bytes ||
            upload_bytes > static_cast<UINT64>((std::numeric_limits<SIZE_T>::max)())) {
            return false;
        }

        D3D12_HEAP_PROPERTIES upload_heap{};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC upload{};
        upload.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upload.Width = upload_bytes;
        upload.Height = 1;
        upload.DepthOrArraySize = 1;
        upload.MipLevels = 1;
        upload.SampleDesc.Count = 1;
        upload.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(state_.device->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &upload,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(entry.upload.ReleaseAndGetAddressOf())))) {
            return false;
        }

        const D3D12_RANGE read_range{};
        void* mapped{};
        if (FAILED(entry.upload->Map(0, &read_range, &mapped)) || mapped == nullptr) return false;
        const auto* source = request.encoded_bytes.data();
        auto* upload_destination = static_cast<std::uint8_t*>(mapped) + footprint.Offset;
        for (std::uint32_t row{}; row < request.height; ++row) {
            std::memcpy(
                upload_destination + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                source + static_cast<std::size_t>(row) * source_row_bytes,
                static_cast<std::size_t>(source_row_bytes));
        }
        const D3D12_RANGE written_range{0, static_cast<SIZE_T>(upload_bytes)};
        entry.upload->Unmap(0, &written_range);

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = entry.texture.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION source_location{};
        source_location.pResource = entry.upload.Get();
        source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source_location.PlacedFootprint = footprint;
        state_.command_list->CopyTextureRegion(
            &destination, 0, 0, 0, &source_location, nullptr);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = entry.texture.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        state_.command_list->ResourceBarrier(1, &barrier);

        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        state_.device->CreateShaderResourceView(entry.texture.Get(), &view, entry.cpu);
        entry.release_fence = state_.next_fence_value;
        entry.upload_fence = state_.next_fence_value;
        entry.upload_byte_size = upload_bytes;
        return true;
    }

    void RebuildFontAtlas(anomaly::UiResourceRegistry& registry) noexcept {
        // ImGui invalidates the prior atlas and releases its descriptor while
        // rebuilding. Defer that mutation until every submitted frame that
        // could reference the old atlas has reached the shared fence.
        if (!font_atlas_dirty_ || !CanRebuildFontAtlas()) return;

        ImGuiIO& io = ImGui::GetIO();
        const bool immediate = ImmediateMode();
        if (immediate) {
            ImGui_ImplDX11_InvalidateDeviceObjects();
        } else {
            ImGui_ImplDX12_InvalidateDeviceObjects();
        }
        static_cast<void>(ConfigurePlatformUiFontAtlas(state_.root));
        bool device_objects_ready = true;
        for (auto& [resource_id, entry] : fonts_) {
            entry.font = nullptr;
            if (!registry.IsResourceLive(resource_id) || entry.failed || entry.bytes.empty() ||
                !IsUsableDimension(entry.size_pixels)) {
                continue;
            }
            ImFontConfig configuration;
            configuration.FontDataOwnedByAtlas = false;
            entry.font = io.Fonts->AddFontFromMemoryTTF(
                entry.bytes.data(), static_cast<int>(entry.bytes.size()), entry.size_pixels,
                &configuration, GlyphRangeFor(*io.Fonts, entry.glyph_range));
            if (entry.font == nullptr) {
                entry.failed = true;
                MarkFontFailedForLeases(registry, resource_id, entry);
            } else {
                entry.failed = false;
            }
        }
        const bool created = immediate ? ImGui_ImplDX11_CreateDeviceObjects()
                                       : ImGui_ImplDX12_CreateDeviceObjects();
        if (!created) device_objects_ready = false;
        if (device_objects_ready) {
            const std::uint64_t generation = registry.DeviceGeneration();
            device_generation_ = generation;
            for (auto& [resource_id, entry] : fonts_) {
                if (entry.font != nullptr && !entry.failed) {
                    MarkFontReadyForLeases(registry, resource_id, entry, generation);
                }
            }
        } else {
            for (auto& [resource_id, entry] : fonts_) {
                if (!entry.failed) {
                    MarkFontFailedForLeases(registry, resource_id, entry);
                    entry.failed = true;
                    entry.font = nullptr;
                }
            }
        }
        retired_fonts_.clear();
        font_atlas_dirty_ = false;
    }

    [[nodiscard]] bool IsFenceComplete(const TextureEntry& entry) const noexcept {
        if (ImmediateMode()) return true;
        return entry.release_fence == 0 ||
            (state_.fence != nullptr && state_.fence->GetCompletedValue() >= entry.release_fence);
    }

    [[nodiscard]] bool IsUploadFenceComplete(const TextureEntry& entry) const noexcept {
        if (ImmediateMode()) return true;
        return entry.upload_fence == 0 ||
            (state_.fence != nullptr && state_.fence->GetCompletedValue() >= entry.upload_fence);
    }

    void ReleaseResidentBytes(const std::uint64_t byte_size) noexcept {
        if (resident_texture_bytes_ >= byte_size) resident_texture_bytes_ -= byte_size;
        else resident_texture_bytes_ = 0;
    }

    void ReleaseUpload(TextureEntry& entry) noexcept {
        entry.upload.Reset();
        ReleaseResidentBytes(entry.upload_byte_size);
        entry.upload_byte_size = 0;
        entry.upload_fence = 0;
    }

    void ReclaimCompletedUploads() noexcept {
        const auto reclaim = [this](TextureEntry& entry) {
            if (entry.upload != nullptr && IsUploadFenceComplete(entry)) ReleaseUpload(entry);
        };
        for (auto& [resource_id, entry] : textures_) {
            static_cast<void>(resource_id);
            reclaim(entry);
        }
        for (TextureEntry& entry : retired_textures_) reclaim(entry);
    }

    void RetireTexture(const std::uint64_t resource_id) noexcept {
        const auto found = textures_.find(resource_id);
        if (found == textures_.end()) return;
        if (IsFenceComplete(found->second)) {
            ReleaseTexture(found->second);
        } else {
            retired_textures_.push_back(std::move(found->second));
        }
        textures_.erase(found);
    }

    void ReclaimRetiredTextures() noexcept {
        for (auto iterator = retired_textures_.begin(); iterator != retired_textures_.end();) {
            if (iterator->upload != nullptr && IsUploadFenceComplete(*iterator)) {
                ReleaseUpload(*iterator);
            }
            if (IsFenceComplete(*iterator)) {
                ReleaseTexture(*iterator);
                iterator = retired_textures_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    void ReleaseTextures(const bool force) noexcept {
        if (!force) {
            for (auto iterator = textures_.begin(); iterator != textures_.end();) {
                if (IsFenceComplete(iterator->second)) {
                    ReleaseTexture(iterator->second);
                    iterator = textures_.erase(iterator);
                } else {
                    ++iterator;
                }
            }
            ReclaimRetiredTextures();
            return;
        }
        for (auto& [resource_id, entry] : textures_) {
            static_cast<void>(resource_id);
            ReleaseTexture(entry);
        }
        textures_.clear();
        for (TextureEntry& entry : retired_textures_) {
            ReleaseTexture(entry);
        }
        retired_textures_.clear();
        resident_texture_bytes_ = 0;
    }

    void ReleaseTexture(TextureEntry& entry) noexcept {
        entry.d3d11_view.Reset();
        entry.d3d11_texture.Reset();
        if (entry.cpu.ptr != 0 || entry.gpu.ptr != 0) {
            static_cast<void>(state_.shader_descriptors.Free(entry.cpu, entry.gpu));
            entry.cpu = {};
            entry.gpu = {};
        }
        ReleaseUpload(entry);
        entry.texture.Reset();
        ReleaseResidentBytes(entry.byte_size);
        entry.byte_size = 0;
        entry.release_fence = 0;
    }

    EmbeddedState& state_;
    std::unordered_map<std::uint64_t, TextureEntry> textures_;
    std::vector<TextureEntry> retired_textures_;
    std::unordered_map<std::uint64_t, FontEntry> fonts_;
    std::vector<FontEntry> retired_fonts_;
    std::uint64_t resident_texture_bytes_{};
    std::uint64_t device_generation_{};
    std::size_t pushed_fonts_{};
    bool font_atlas_dirty_{};
};

}  // namespace

std::shared_ptr<anomaly::UiResourceRenderBackend>
CreateEmbeddedUiResourceRenderBackend(EmbeddedState& state) noexcept {
    try {
        return std::make_shared<EmbeddedUiResourceRenderBackend>(state);
    } catch (...) {
        return {};
    }
}

}  // namespace ue5mem::embedded
