#include "anomaly/liquid_glass.hpp"

#include "anomaly/platform_ui_theme.hpp"

#include <imgui_internal.h>

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <d3dcommon.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <deque>
#include <iterator>
#include <memory>
#include <unordered_map>
#include <vector>

// Bytecode compiled from src/ui/shaders by fxc at build time. Both hosts draw
// with it: D3D12 accepts the same shader model 5.0 bytecode D3D11 does.
#include "anomaly/shaders/kGlassBlurPs.h"
#include "anomaly/shaders/kGlassDownsamplePs.h"
#include "anomaly/shaders/kGlassFullScreenVs.h"
#include "anomaly/shaders/kGlassSurfacePs.h"
#include "anomaly/shaders/kGlassSurfaceVs.h"

namespace anomaly::glass {
namespace {

using Microsoft::WRL::ComPtr;

struct alignas(16) GlassConstants final {
    float screen[4]{};
    float rect[4]{};
    float shape[4]{};
    float tint[4]{};
    float optics[4]{};
    float light[4]{};
    float shadow[4]{};
    float extra[4]{};
    float edge[4]{};
    float sector[4]{};  // ring center, mid angle, half aperture; zero aperture = box
    float ring[4]{};    // inner radius, outer radius
};

struct alignas(16) PassConstants final {
    float texel[4]{};
    float pass[4]{};
};

struct RenderTarget final {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> view;
    ComPtr<ID3D11ShaderResourceView> resource;
    UINT width{};
    UINT height{};
};

struct Renderer final {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> full_screen;
    ComPtr<ID3D11VertexShader> quad;
    ComPtr<ID3D11PixelShader> downsample;
    ComPtr<ID3D11PixelShader> blur;
    ComPtr<ID3D11PixelShader> glass;
    ComPtr<ID3D11Buffer> glass_constants;
    ComPtr<ID3D11Buffer> pass_constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11BlendState> opaque;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> pass_rasterizer;
    ComPtr<ID3D11DepthStencilState> depth_off;
    ComPtr<ID3D11Texture2D> backdrop;
    ComPtr<ID3D11ShaderResourceView> backdrop_view;
    UINT backdrop_width{};
    UINT backdrop_height{};
    DXGI_FORMAT backdrop_format{DXGI_FORMAT_UNKNOWN};
    RenderTarget half;
    RenderTarget half_scratch;
    RenderTarget quarter;
    RenderTarget scratch;
    int captured_frame{-1};
    std::array<ComPtr<ID3D11Texture2D>, 4> desktop_layer;
    std::array<ComPtr<ID3D11ShaderResourceView>, 3> desktop_views;
    bool desktop_sharp{};
    bool desktop_blur{};
};

std::unique_ptr<Renderer> g_renderer;
// Defined with its backend further down; the shared callbacks above only need
// the declaration.
struct D3d12Renderer;
std::unique_ptr<D3d12Renderer> g_d3d12;
bool g_reduce_transparency{};
// Hosts that should keep the palette tint (the shells) leave this on; a neutral
// host such as the launcher turns it off.
bool g_palette_tint{true};
bool g_reduce_motion{};

enum class CommandKind : std::uint8_t { Capture, Surface };

struct Command final {
    CommandKind kind{CommandKind::Surface};
    int frame{};
    bool clip{true};
    bool blur{true};
    bool deep{false};
    GlassConstants constants;
    ImRect sampled_region;
    ImRect blurred_region;
    bool bounded_copy{};
};

// The same schedule drives both backends and the calculation of each filter's input pixels.
enum BackdropLevel : std::size_t { Sharp, Half, HalfScratch, Quarter, QuarterScratch, LevelCount };
struct BlurPass final {
    BackdropLevel source;
    BackdropLevel target;
    float dx, dy, spread;  // zero spread selects the 2x2 downsample
};
constexpr BlurPass kBlurPasses[]{
    {Sharp, Half, 0, 0, 0}, {Half, HalfScratch, 1, 0, 1}, {HalfScratch, Half, 0, 1, 1},
    {Half, Quarter, 0, 0, 0},
    {Quarter, QuarterScratch, 1, 0, 1.2f}, {QuarterScratch, Quarter, 0, 1, 1.2f},
    {Quarter, QuarterScratch, 1, 0, 1.8f}, {QuarterScratch, Quarter, 0, 1, 1.8f},
    {Quarter, QuarterScratch, 1, 0, 2.6f}, {QuarterScratch, Quarter, 0, 1, 2.6f},
    {Quarter, QuarterScratch, 1, 0, 3.8f}, {QuarterScratch, Quarter, 0, 1, 3.8f},
};
struct Extent final { UINT width, height; };
struct CapturePlan final {
    RECT copy{};
    std::array<RECT, std::size(kBlurPasses)> passes{};
    std::size_t count{};
};

bool HasPixels(const RECT& r) noexcept { return r.left < r.right && r.top < r.bottom; }

// Project the destination's pixel edges into the source texture. Keep the viewport and
// UVs at their original sizes, including for odd dimensions. Only the scissor changes.
RECT PixelBounds(const ImRect& area, const Extent from, const Extent to,
    const float border_x = 0.0f, const float border_y = 0.0f) noexcept {
    if (area.Min.x >= area.Max.x || area.Min.y >= area.Max.y) return {};
    const float sx = static_cast<float>(to.width) / static_cast<float>(from.width);
    const float sy = static_cast<float>(to.height) / static_cast<float>(from.height);
    const auto pixel = [](const float value, const UINT limit) {
        return static_cast<LONG>(std::clamp(value, 0.0f, static_cast<float>(limit)));
    };
    return {pixel(std::floor(area.Min.x * sx - border_x), to.width),
        pixel(std::floor(area.Min.y * sy - border_y), to.height),
        pixel(std::ceil(area.Max.x * sx + border_x), to.width),
        pixel(std::ceil(area.Max.y * sy + border_y), to.height)};
}

CapturePlan PlanCapture(const Command& command, const Extent full, const Extent half,
    const Extent quarter) noexcept {
    CapturePlan plan;
    const Extent sizes[LevelCount]{full, half, half, quarter, quarter};
    plan.count = command.blur ? (command.deep ? std::size(kBlurPasses) : 8) : 0;
    if (!command.bounded_copy) {
        plan.copy = {0, 0, static_cast<LONG>(full.width), static_cast<LONG>(full.height)};
        for (std::size_t i = 0; i < plan.count; ++i) {
            const Extent target = sizes[kBlurPasses[i].target];
            plan.passes[i] = {0, 0, static_cast<LONG>(target.width), static_cast<LONG>(target.height)};
        }
        return plan;
    }
    RECT needed[LevelCount]{};
    needed[Sharp] = PixelBounds(command.sampled_region, full, full);
    if (plan.count) {
        // The surface shader samples each level with a bilinear sampler.
        needed[Half] = PixelBounds(command.blurred_region, full, half, 1, 1);
        needed[Quarter] = PixelBounds(command.blurred_region, full, quarter, 1, 1);
    }
    for (std::size_t i = plan.count; i-- > 0;) {
        const BlurPass& pass = kBlurPasses[i];
        const RECT region = needed[pass.target];
        plan.passes[i] = region;
        needed[pass.target] = {}; // An earlier write to this ping-pong level has different readers.
        if (!HasPixels(region)) continue;
        // ps_down samples +/-1 texel on both axes. ps_blur's outermost sample is
        // 3.23077 * spread on one axis. Include the footprint of linear filtering.
        constexpr float outer_tap = 3.231f;
        const float x = pass.spread ? outer_tap * pass.spread * pass.dx : 1.0f;
        const float y = pass.spread ? outer_tap * pass.spread * pass.dy : 1.0f;
        const RECT reads = PixelBounds(ImRect(static_cast<float>(region.left), static_cast<float>(region.top),
            static_cast<float>(region.right), static_cast<float>(region.bottom)),
            sizes[pass.target], sizes[pass.source], x + 1.0f, y + 1.0f);
        UnionRect(&needed[pass.source], &needed[pass.source], &reads);
    }
    plan.copy = needed[Sharp];
    return plan;
}

void D3d12Capture(const Command& command) noexcept;
void D3d12Surface(const Command& command, const ImDrawCmd& draw_command) noexcept;

// Callback payloads live here until the frame that recorded them has been
// rendered; the next frame's first command recycles the storage. A deque keeps
// the addresses stable while it grows.
std::deque<Command> g_commands;
std::size_t g_command_count{};
int g_commands_frame{-1};

struct WindowSurface final {
    int frame{};
    SurfaceOptions options;
};

std::unordered_map<ImGuiID, WindowSurface> g_window_surfaces;
std::vector<std::unique_ptr<ImDrawList>> g_underlays;

Command& NewCommand() {
    const int frame = ImGui::GetFrameCount();
    if (frame != g_commands_frame) {
        g_command_count = 0;
        g_commands_frame = frame;
    }
    if (g_command_count == g_commands.size()) g_commands.emplace_back();
    Command& command = g_commands[g_command_count++];
    command = {};
    command.frame = frame;
    return command;
}

bool CreateTarget(ID3D11Device* device, RenderTarget& target,
    const UINT width, const UINT height) noexcept {
    if (target.texture != nullptr && target.width == width && target.height == height) {
        return true;
    }
    target = {};
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &target.texture)) ||
        FAILED(device->CreateRenderTargetView(target.texture.Get(), nullptr, &target.view)) ||
        FAILED(device->CreateShaderResourceView(
            target.texture.Get(), nullptr, &target.resource))) {
        target = {};
        return false;
    }
    target.width = width;
    target.height = height;
    return true;
}

bool EnsureTargets(Renderer& renderer, const D3D11_TEXTURE2D_DESC& source) noexcept {
    if (renderer.backdrop == nullptr || renderer.backdrop_width != source.Width ||
        renderer.backdrop_height != source.Height ||
        renderer.backdrop_format != source.Format) {
        renderer.backdrop.Reset();
        renderer.backdrop_view.Reset();
        D3D11_TEXTURE2D_DESC description{};
        description.Width = source.Width;
        description.Height = source.Height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = source.Format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(renderer.device->CreateTexture2D(
                &description, nullptr, &renderer.backdrop)) ||
            FAILED(renderer.device->CreateShaderResourceView(
                renderer.backdrop.Get(), nullptr, &renderer.backdrop_view))) {
            renderer.backdrop.Reset();
            renderer.backdrop_view.Reset();
            return false;
        }
        renderer.backdrop_width = source.Width;
        renderer.backdrop_height = source.Height;
        renderer.backdrop_format = source.Format;
    }
    const UINT half_width = (std::max)(1U, (source.Width + 1U) / 2U);
    const UINT half_height = (std::max)(1U, (source.Height + 1U) / 2U);
    const UINT quarter_width = (std::max)(1U, (source.Width + 3U) / 4U);
    const UINT quarter_height = (std::max)(1U, (source.Height + 3U) / 4U);
    return CreateTarget(renderer.device.Get(), renderer.half, half_width, half_height) &&
        CreateTarget(renderer.device.Get(), renderer.half_scratch, half_width, half_height) &&
        CreateTarget(renderer.device.Get(), renderer.quarter, quarter_width, quarter_height) &&
        CreateTarget(renderer.device.Get(), renderer.scratch, quarter_width, quarter_height);
}

template <typename T>
void Upload(ID3D11DeviceContext* context, ID3D11Buffer* buffer, const T& value) noexcept {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, &value, sizeof(T));
        context->Unmap(buffer, 0);
    }
}

void CaptureCallback(const ImDrawList*, const ImDrawCmd* draw_command) {
    const auto* command = static_cast<const Command*>(draw_command->UserCallbackData);
    if (command == nullptr) return;
    if (g_d3d12 != nullptr) {
        D3d12Capture(*command);
        return;
    }
    Renderer* const renderer = g_renderer.get();
    if (renderer == nullptr) return;
    ID3D11DeviceContext* const context = renderer->context.Get();
    ComPtr<ID3D11RenderTargetView> output;
    ComPtr<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(1, &output, &depth);
    if (output == nullptr) return;
    ComPtr<ID3D11Resource> resource;
    output->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (resource == nullptr || FAILED(resource.As(&texture))) return;
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (description.SampleDesc.Count != 1 || !EnsureTargets(*renderer, description)) return;

    const CapturePlan plan = PlanCapture(*command, {description.Width, description.Height},
        {renderer->half.width, renderer->half.height}, {renderer->quarter.width, renderer->quarter.height});
    if (!HasPixels(plan.copy)) return;
    ID3D11ShaderResourceView* const unbound[3]{};
    context->PSSetShaderResources(0, 3, unbound);
    const RECT& r = plan.copy;
    if (r.left == 0 && r.top == 0 && r.right == static_cast<LONG>(description.Width) &&
        r.bottom == static_cast<LONG>(description.Height)) {
        context->CopyResource(renderer->backdrop.Get(), texture.Get());
    } else {
        const D3D11_BOX box{static_cast<UINT>(r.left), static_cast<UINT>(r.top), 0,
            static_cast<UINT>(r.right), static_cast<UINT>(r.bottom), 1};
        context->CopySubresourceRegion(renderer->backdrop.Get(), 0, box.left, box.top, 0, texture.Get(), 0, &box);
    }
    renderer->desktop_sharp = false;
    if (!command->blur) {
        renderer->captured_frame = command->frame;
        return;
    }

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(renderer->full_screen.Get(), nullptr, 0);
    context->PSSetSamplers(0, 1, renderer->sampler.GetAddressOf());
    const float blend_factor[4]{};
    context->OMSetBlendState(renderer->opaque.Get(), blend_factor, 0xffffffffU);
    context->OMSetDepthStencilState(renderer->depth_off.Get(), 0);
    context->RSSetState(renderer->pass_rasterizer.Get());
    context->PSSetConstantBuffers(0, 1, renderer->pass_constants.GetAddressOf());
    const RenderTarget* const levels[LevelCount]{nullptr, &renderer->half, &renderer->half_scratch,
        &renderer->quarter, &renderer->scratch};
    for (std::size_t i = 0; i < plan.count; ++i) {
        if (!HasPixels(plan.passes[i])) continue;
        const BlurPass& pass = kBlurPasses[i];
        const RenderTarget* source = levels[pass.source];
        const RenderTarget& target = *levels[pass.target];
        ID3D11ShaderResourceView* srv = source ? source->resource.Get() : renderer->backdrop_view.Get();
        context->PSSetShaderResources(0, 3, unbound);
        context->OMSetRenderTargets(1, target.view.GetAddressOf(), nullptr);
        const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(target.width),
            static_cast<float>(target.height), 0.0f, 1.0f};
        context->RSSetViewports(1, &viewport);
        PassConstants constants;
        constants.texel[0] = 1.0f / static_cast<float>(source ? source->width : description.Width);
        constants.texel[1] = 1.0f / static_cast<float>(source ? source->height : description.Height);
        constants.texel[2] = pass.dx;
        constants.texel[3] = pass.dy;
        constants.pass[0] = pass.spread;
        Upload(context, renderer->pass_constants.Get(), constants);
        context->PSSetShader(pass.spread ? renderer->blur.Get() : renderer->downsample.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, &srv);
        context->RSSetScissorRects(1, &plan.passes[i]);
        context->Draw(3, 0);
    }
    context->PSSetShaderResources(0, 3, unbound);
    context->OMSetRenderTargets(1, output.GetAddressOf(), depth.Get());
    renderer->captured_frame = command->frame;
    renderer->desktop_blur = false;
}

void SurfaceCallback(const ImDrawList*, const ImDrawCmd* draw_command) {
    const auto* const recorded = static_cast<const Command*>(draw_command->UserCallbackData);
    if (recorded == nullptr) return;
    if (g_d3d12 != nullptr) {
        D3d12Surface(*recorded, *draw_command);
        return;
    }
    Renderer* const renderer = g_renderer.get();
    const Command* const command = recorded;
    if (renderer == nullptr) return;
    ID3D11DeviceContext* const context = renderer->context.Get();
    UINT viewport_count = 1;
    D3D11_VIEWPORT viewport{};
    context->RSGetViewports(&viewport_count, &viewport);
    if (viewport_count == 0 || viewport.Width <= 0.0f || viewport.Height <= 0.0f) return;
    GlassConstants constants = command->constants;
    constants.screen[0] = viewport.Width;
    constants.screen[1] = viewport.Height;
    constants.screen[2] = 1.0f / viewport.Width;
    constants.screen[3] = 1.0f / viewport.Height;
    const bool backdrop_ready = renderer->captured_frame == command->frame &&
        renderer->backdrop_view != nullptr &&
        static_cast<float>(renderer->backdrop_width) == viewport.Width &&
        static_cast<float>(renderer->backdrop_height) == viewport.Height;
    if (!backdrop_ready) {
        // Nothing was captured for this frame: draw the tint alone rather than
        // refracting a stale picture.
        constants.tint[3] = 1.0f;
        constants.light[3] = 1.0f;
    }
    Upload(context, renderer->glass_constants.Get(), constants);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->VSSetShader(renderer->quad.Get(), nullptr, 0);
    context->PSSetShader(renderer->glass.Get(), nullptr, 0);
    context->VSSetConstantBuffers(0, 1, renderer->glass_constants.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, renderer->glass_constants.GetAddressOf());
    ID3D11ShaderResourceView* resources[3]{};
    if (backdrop_ready) {
        resources[0] = renderer->desktop_sharp ? renderer->desktop_views[0].Get() : renderer->backdrop_view.Get();
        resources[1] = renderer->desktop_blur ? renderer->desktop_views[2].Get() : renderer->quarter.resource.Get();
        resources[2] = renderer->desktop_blur ? renderer->desktop_views[1].Get() : renderer->half.resource.Get();
    }
    context->PSSetShaderResources(0, 3, resources);
    context->PSSetSamplers(0, 1, renderer->sampler.GetAddressOf());
    const float blend_factor[4]{};
    context->OMSetBlendState(renderer->blend.Get(), blend_factor, 0xffffffffU);
    D3D11_RECT scissor{0, 0, static_cast<LONG>(viewport.Width),
        static_cast<LONG>(viewport.Height)};
    if (command->clip) {
        scissor.left = (std::max)(0L, static_cast<LONG>(draw_command->ClipRect.x));
        scissor.top = (std::max)(0L, static_cast<LONG>(draw_command->ClipRect.y));
        scissor.right = (std::min)(scissor.right, static_cast<LONG>(draw_command->ClipRect.z));
        scissor.bottom = (std::min)(scissor.bottom, static_cast<LONG>(draw_command->ClipRect.w));
        if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) return;
    }
    context->RSSetScissorRects(1, &scissor);
    context->Draw(4, 0);
    ID3D11ShaderResourceView* const unbound[3]{};
    context->PSSetShaderResources(0, 3, unbound);
}


// ---------------------------------------------------------------------------
// D3D12
// ---------------------------------------------------------------------------
// The same material on a host whose back buffer is a D3D12 resource. Every
// command here is recorded into the list the overlay's own UI is being recorded
// on, which is what lets a surface sit under the window that owns it: the two
// callbacks below run from inside ImGui_ImplDX12_RenderDrawData.
//
// The shaders are the shipped ones, so this backend has to supply exactly what
// they expect: the sharp capture and the two blurred levels the D3D11 host
// builds (light at half resolution, heavy at quarter). The chain below mirrors
// that host pass for pass.
//
// One difference is forced by the platform: the game's back buffer carries no
// meaningful alpha, and the material treats its backdrop as premultiplied. The
// views below therefore map alpha to a constant one, which makes the sampled
// backdrop opaque exactly as it is under the D3D11 host.

struct D3d12Level final {
    ComPtr<ID3D12Resource> texture;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    UINT width{};
    UINT height{};
};

// The descriptor layout of one frame. A root table is resolved as a whole
// range, and the GPU reads the heap when the list executes rather than when the
// command was recorded, so a table can only be shared by draws that sample the
// same texture. Every texture the chain touches therefore owns a fixed slot for
// the whole frame: several captures happen in one frame, all of them re-blur the
// same five levels in order, and rewriting a slot per pass would hand the last
// pass's source to every earlier pass in the queue.
// The first slots follow BackdropLevel so every pass can bind its source directly.
// The material's own backdrop: the sharp capture, its heavy blur and its light
// blur, the same three the D3D11 host binds.
constexpr std::size_t kGlassSurfaceFirst = LevelCount;
constexpr std::size_t kGlassDescriptorCount = kGlassSurfaceFirst + 3;

struct D3d12Renderer final {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> glass;
    ComPtr<ID3D12PipelineState> downsample;
    ComPtr<ID3D12PipelineState> blur;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    UINT rtv_stride{};
    // Root descriptors cannot address a Texture2D, so the textures live in a
    // shader-visible heap: every chain pass owns a table of its own for the one
    // texture it samples, and the last three descriptors are the material's
    // backdrop -- the sharp capture, its heavy blur and its light blur, the
    // same three the D3D11 host binds.
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    UINT srv_stride{};
    D3D12_GPU_DESCRIPTOR_HANDLE srv_base{};
    D3D12_GPU_DESCRIPTOR_HANDLE surface_table{};
    ComPtr<ID3D12Resource> capture;
    D3D12_CPU_DESCRIPTOR_HANDLE capture_rtv{};
    D3d12Level half;
    D3d12Level half_scratch;
    D3d12Level quarter;
    D3d12Level quarter_scratch;
    UINT width{};
    UINT height{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    ID3D12GraphicsCommandList* list{};
    ID3D12Resource* back_buffer{};
    D3D12_CPU_DESCRIPTOR_HANDLE frame_rtv{};
    ID3D12DescriptorHeap* host_heap{};
    int captured_frame{-1};
};

[[nodiscard]] D3D12_RESOURCE_BARRIER MakeBarrier(ID3D12Resource* const resource,
    const D3D12_RESOURCE_STATES before, const D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

// Alpha forced to one: the material's backdrop is opaque by contract, and the
// overlay's back buffer does not carry that in its alpha channel.
[[nodiscard]] D3D12_SHADER_RESOURCE_VIEW_DESC OpaqueTextureView(
    const DXGI_FORMAT format) noexcept {
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        0, 1, 2, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
    view.Texture2D.MipLevels = 1;
    return view;
}

[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE SrvHandle(
    const D3d12Renderer& renderer, const std::size_t slot) noexcept {
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    handle.ptr = renderer.srv_heap->GetCPUDescriptorHandleForHeapStart().ptr +
        static_cast<SIZE_T>(renderer.srv_stride) * slot;
    return handle;
}

// The table one chain pass binds: the first descriptor is the level it samples,
// the other two are never read.
[[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE PassTable(
    const D3d12Renderer& renderer, const std::size_t slot) noexcept {
    D3D12_GPU_DESCRIPTOR_HANDLE handle{};
    handle.ptr = renderer.srv_base.ptr + static_cast<UINT64>(renderer.srv_stride) * slot;
    return handle;
}

[[nodiscard]] ComPtr<ID3D12Resource> CreateOpaqueTarget(ID3D12Device* const device,
    const UINT width, const UINT height, const DXGI_FORMAT format) noexcept {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> resource;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&resource)))) {
        return {};
    }
    return resource;
}



[[nodiscard]] bool EnsureCapture(D3d12Renderer& renderer, const UINT width,
    const UINT height, const DXGI_FORMAT format) noexcept {
    if (renderer.capture != nullptr && renderer.width == width &&
        renderer.height == height && renderer.format == format) {
        return true;
    }
    renderer.capture.Reset();
    renderer.half = {};
    renderer.half_scratch = {};
    renderer.quarter = {};
    renderer.quarter_scratch = {};
    renderer.width = width;
    renderer.height = height;
    renderer.format = format;

    renderer.capture = CreateOpaqueTarget(renderer.device.Get(), width, height, format);
    if (renderer.capture == nullptr) return false;
    // Render-target views for the level chain, allocated from our own heap.
    const UINT half_width = (std::max)(1U, width / 2U);
    const UINT half_height = (std::max)(1U, height / 2U);
    const UINT quarter_width = (std::max)(1U, half_width / 2U);
    const UINT quarter_height = (std::max)(1U, half_height / 2U);
    const auto make_level = [&](D3d12Level& level, const UINT level_width,
                                const UINT level_height,
                                const UINT rtv_slot) -> bool {
        level.texture = CreateOpaqueTarget(
            renderer.device.Get(), level_width, level_height, format);
        if (level.texture == nullptr) return false;
        level.width = level_width;
        level.height = level_height;
        level.rtv.ptr = renderer.rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr +
            static_cast<SIZE_T>(renderer.rtv_stride) * rtv_slot;
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        renderer.device->CreateRenderTargetView(level.texture.Get(), &view, level.rtv);
        return true;
    };
    // slot 0 holds the capture's own target (unused after the copy), 1..4 the
    // four chain levels.
    if (!make_level(renderer.half, half_width, half_height, 1) ||
        !make_level(renderer.half_scratch, half_width, half_height, 2) ||
        !make_level(renderer.quarter, quarter_width, quarter_height, 3) ||
        !make_level(renderer.quarter_scratch, quarter_width, quarter_height, 4)) {
        return false;
    }
    const auto store = [&](const std::size_t slot, ID3D12Resource* const texture) {
        const D3D12_SHADER_RESOURCE_VIEW_DESC view = OpaqueTextureView(format);
        renderer.device->CreateShaderResourceView(texture, &view, SrvHandle(renderer, slot));
    };
    store(Sharp, renderer.capture.Get());
    store(Half, renderer.half.texture.Get());
    store(HalfScratch, renderer.half_scratch.texture.Get());
    store(Quarter, renderer.quarter.texture.Get());
    store(QuarterScratch, renderer.quarter_scratch.texture.Get());
    store(kGlassSurfaceFirst, renderer.capture.Get());
    store(kGlassSurfaceFirst + 1, renderer.quarter.texture.Get());
    store(kGlassSurfaceFirst + 2, renderer.half.texture.Get());
    return true;
}

// Each filter pass binds its source's fixed descriptor and draws the required target pixels.
void RunPass(D3d12Renderer& renderer, const BlurPass& pass, D3d12Level& target,
    const Extent source, const RECT& region) noexcept {
    ID3D12GraphicsCommandList* const list = renderer.list;
    PassConstants constants;
    constants.texel[0] = 1.0F / static_cast<float>(source.width);
    constants.texel[1] = 1.0F / static_cast<float>(source.height);
    constants.texel[2] = pass.dx;
    constants.texel[3] = pass.dy;
    constants.pass[0] = pass.spread;
    list->SetPipelineState(pass.spread ? renderer.blur.Get() : renderer.downsample.Get());
    list->SetGraphicsRoot32BitConstants(0, sizeof(constants) / sizeof(UINT), &constants, 0);
    list->SetGraphicsRootDescriptorTable(1, PassTable(renderer, pass.source));

    const D3D12_RESOURCE_BARRIER barriers[]{
        MakeBarrier(target.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET)};
    list->ResourceBarrier(1, barriers);
    list->OMSetRenderTargets(1, &target.rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(target.width),
        static_cast<float>(target.height), 0.0F, 1.0F};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &region);
    list->DrawInstanced(3, 1, 0, 0);
    const D3D12_RESOURCE_BARRIER back[]{
        MakeBarrier(target.texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
    list->ResourceBarrier(1, back);
}

// ImGui's D3D12 backend binds its shader-visible heap once, around the whole
// draw data, and neither its per-command path nor ImDrawCallback_ResetRenderState
// ever binds it again. Anything that switches heaps therefore has to hand the
// host's own back, or every later draw -- labels, glyphs, images -- resolves its
// texture through the material's heap.
void RestoreHostHeap(D3d12Renderer& renderer) noexcept {
    if (renderer.list == nullptr || renderer.host_heap == nullptr) return;
    ID3D12DescriptorHeap* const heaps[]{renderer.host_heap};
    renderer.list->SetDescriptorHeaps(1, heaps);
}

void D3d12Capture(const Command& command) noexcept {
    D3d12Renderer* const renderer = g_d3d12.get();
    if (renderer == nullptr || renderer->list == nullptr || renderer->back_buffer == nullptr) return;
    const D3D12_RESOURCE_DESC description = renderer->back_buffer->GetDesc();
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;
    if (!EnsureCapture(*renderer, static_cast<UINT>(description.Width),
            static_cast<UINT>(description.Height), description.Format)) {
        return;
    }
    const Extent full{renderer->width, renderer->height};
    const CapturePlan plan = PlanCapture(command, full, {renderer->half.width, renderer->half.height},
        {renderer->quarter.width, renderer->quarter.height});
    if (!HasPixels(plan.copy)) return;
    ID3D12GraphicsCommandList* const list = renderer->list;
    D3D12_RESOURCE_BARRIER barriers[]{
        MakeBarrier(renderer->back_buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
        MakeBarrier(renderer->capture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
    list->ResourceBarrier(2, barriers);
    const RECT& r = plan.copy;
    if (r.left == 0 && r.top == 0 && r.right == static_cast<LONG>(full.width) &&
        r.bottom == static_cast<LONG>(full.height)) {
        list->CopyResource(renderer->capture.Get(), renderer->back_buffer);
    } else {
        const D3D12_BOX box{static_cast<UINT>(r.left), static_cast<UINT>(r.top), 0,
            static_cast<UINT>(r.right), static_cast<UINT>(r.bottom), 1};
        const D3D12_TEXTURE_COPY_LOCATION src{renderer->back_buffer, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        const D3D12_TEXTURE_COPY_LOCATION dst{renderer->capture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        list->CopyTextureRegion(&dst, box.left, box.top, 0, &src, &box);
    }
    for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(2, barriers);
    renderer->captured_frame = command.frame;
    if (!command.blur) {
        list->OMSetRenderTargets(1, &renderer->frame_rtv, FALSE, nullptr);
        return;
    }
    ID3D12DescriptorHeap* const heaps[]{renderer->srv_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(renderer->root_signature.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3d12Level* const levels[LevelCount]{nullptr, &renderer->half, &renderer->half_scratch,
        &renderer->quarter, &renderer->quarter_scratch};
    for (std::size_t i = 0; i < plan.count; ++i) {
        if (!HasPixels(plan.passes[i])) continue;
        const BlurPass& pass = kBlurPasses[i];
        const D3d12Level* source = levels[pass.source];
        RunPass(*renderer, pass, *levels[pass.target], source ? Extent{source->width, source->height} : full,
            plan.passes[i]);
    }
    list->OMSetRenderTargets(1, &renderer->frame_rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(renderer->width),
        static_cast<float>(renderer->height), 0.0F, 1.0F};
    list->RSSetViewports(1, &viewport);
    RestoreHostHeap(*renderer);
}


void D3d12Surface(const Command& command, const ImDrawCmd& draw_command) noexcept {
    D3d12Renderer* const renderer = g_d3d12.get();
    if (renderer == nullptr || renderer->list == nullptr || renderer->capture == nullptr ||
        renderer->width == 0 || renderer->height == 0) {
        return;
    }
    ID3D12GraphicsCommandList* const list = renderer->list;
    GlassConstants constants = command.constants;
    constants.screen[0] = static_cast<float>(renderer->width);
    constants.screen[1] = static_cast<float>(renderer->height);
    constants.screen[2] = 1.0F / static_cast<float>(renderer->width);
    constants.screen[3] = 1.0F / static_cast<float>(renderer->height);
    if (renderer->captured_frame != command.frame) {
        // Nothing was captured for this frame: the tint alone, as on D3D11.
        constants.tint[3] = 1.0F;
        constants.light[3] = 1.0F;
    }
    D3D12_RECT scissor{0, 0, static_cast<LONG>(renderer->width),
        static_cast<LONG>(renderer->height)};
    if (command.clip) {
        scissor.left = (std::max)(0L, static_cast<LONG>(draw_command.ClipRect.x));
        scissor.top = (std::max)(0L, static_cast<LONG>(draw_command.ClipRect.y));
        scissor.right = (std::min)(scissor.right, static_cast<LONG>(draw_command.ClipRect.z));
        scissor.bottom = (std::min)(scissor.bottom, static_cast<LONG>(draw_command.ClipRect.w));
        if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) return;
    }
    ID3D12DescriptorHeap* const heaps[]{renderer->srv_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(renderer->root_signature.Get());
    list->SetPipelineState(renderer->glass.Get());
    list->SetGraphicsRoot32BitConstants(0, sizeof(constants) / sizeof(UINT), &constants, 0);
    list->SetGraphicsRootDescriptorTable(1, renderer->surface_table);
    list->OMSetRenderTargets(1, &renderer->frame_rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(renderer->width),
        static_cast<float>(renderer->height), 0.0F, 1.0F};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    list->DrawInstanced(4, 1, 0, 0);
    // Hand the host's heap back: every ImGui draw after this one resolves its
    // texture through it.
    RestoreHostHeap(*renderer);
}



ImVec4 Mix(const ImVec4& from, const ImVec4& to, const float amount) noexcept {
    return {from.x + (to.x - from.x) * amount, from.y + (to.y - from.y) * amount,
        from.z + (to.z - from.z) * amount, from.w + (to.w - from.w) * amount};
}

ImVec4 ToVec4(const ue5mem::PlatformUiColor& color) noexcept {
    return {color.red, color.green, color.blue, color.alpha};
}

struct Look final {
    ImVec4 tint{};
    float tint_strength{};
    float refraction{};
    float chroma{};
    float blur_mix{1.0f};
    float saturation{1.0f};
    float rim{1.0f};
    float lift{};
    float minimum_alpha{};
    float shadow_alpha{};
    float shadow_offset{};
    float shadow_blur{};
    float gloss{};
    float bevel{12.0f};
    float glow{};
    float magnify{};
    float contact{};
    // How much the inner shoulder darkens, mostly on the side facing away
    // from the light. This is what gives a pane its thickness.
    float shade{};
    // The sharpest blur the shoulder may sample. A deeply frosted face needs a
    // shoulder that is still soft, or the bevel turns into a hard ring inside
    // the pane.
    float rim_blur{0.25f};
    float fallback_alpha{0.9f};
};

// Over a flat wallpaper there is little for refraction to bend, so the glass
// has to be carried by its edge: the rim line, the glow inside the bevel and
// the gloss on the upper half. Shadows stay faint and only floating panes get
// one at all.
Look ResolveLook(const SurfaceOptions& options) noexcept {
    const bool dark = DarkAppearance();
    const ImVec4 surface = ToVec4(ue5mem::PlatformUiSurfaceColor());
    Look look;
    look.tint = !g_palette_tint
        ? (dark ? ImVec4(0.10f, 0.10f, 0.10f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f))
        : (dark ? Mix(surface, ImVec4(0.0f, 0.0f, 0.0f, 1.0f), 0.15f)
                : Mix(surface, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.70f));
    look.tint.w = 1.0f;
    // blur_mix is the face blur: 0 sharp, 1 light, 2 heavy.
    switch (options.variant) {
    case Variant::Regular:
        look.tint_strength = dark ? 0.26f : 0.12f;
        look.refraction = 40.0f;
        look.chroma = 0.14f;
        look.blur_mix = 1.45f;
        look.saturation = 1.80f;
        look.rim = dark ? 0.75f : 1.0f;
        look.lift = dark ? 0.01f : 0.03f;
        look.minimum_alpha = 0.90f;
        look.shadow_alpha = dark ? 0.20f : 0.05f;
        look.shadow_offset = 3.0f;
        look.shadow_blur = 14.0f;
        look.gloss = dark ? 0.03f : 0.07f;
        look.bevel = 26.0f;
        look.glow = dark ? 0.12f : 0.22f;
        look.magnify = 0.0f;
        look.contact = dark ? 0.20f : 0.07f;
        look.shade = dark ? 0.16f : 0.10f;
        look.fallback_alpha = 0.84f;
        break;
    case Variant::Clear:
        look.tint_strength = 0.02f;
        look.refraction = 22.0f;
        look.chroma = 0.20f;
        look.blur_mix = 0.15f;
        look.saturation = 1.25f;
        look.rim = 1.0f;
        look.lift = 0.02f;
        look.minimum_alpha = 0.0f;
        look.shadow_alpha = dark ? 0.18f : 0.06f;
        look.shadow_offset = 2.0f;
        look.shadow_blur = 6.0f;
        look.gloss = 0.10f;
        look.bevel = 12.0f;
        look.glow = 0.24f;
        look.magnify = 0.08f;
        look.contact = 0.10f;
        look.shade = 0.10f;
        look.fallback_alpha = 0.35f;
        break;
    case Variant::Material:
        look.tint_strength = dark ? 0.38f : 0.24f;
        look.refraction = 26.0f;
        look.chroma = 0.08f;
        look.blur_mix = 1.85f;
        look.saturation = 1.50f;
        look.rim = dark ? 0.55f : 0.75f;
        look.lift = dark ? 0.01f : 0.05f;
        look.minimum_alpha = 0.94f;
        look.shadow_alpha = 0.0f;
        look.gloss = dark ? 0.02f : 0.04f;
        look.bevel = 20.0f;
        look.glow = dark ? 0.08f : 0.14f;
        look.magnify = 0.0f;
        look.contact = dark ? 0.16f : 0.06f;
        look.shade = dark ? 0.12f : 0.07f;
        look.fallback_alpha = 0.90f;
        break;
    case Variant::Thumb:
        // The selection lens. It is drawn under its label, so it can bend and
        // magnify the glass it sits on freely: a bright two-sided rim, a glow
        // in the lit corner, a shaded far shoulder and a gloss on the face.
        look.tint = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        look.tint_strength = dark ? 0.10f : 0.30f;
        look.refraction = 16.0f;
        look.chroma = 0.30f;
        look.blur_mix = 0.0f;
        look.saturation = 1.30f;
        look.rim = dark ? 0.90f : 1.20f;
        look.lift = dark ? 0.03f : 0.10f;
        look.minimum_alpha = 0.0f;
        look.shadow_alpha = 0.0f;
        look.gloss = dark ? 0.06f : 0.18f;
        look.bevel = 12.0f;
        look.glow = dark ? 0.25f : 0.45f;
        look.magnify = 0.05f;
        look.contact = 0.0f;
        look.shade = dark ? 0.18f : 0.12f;
        look.fallback_alpha = 0.55f;
        break;
    }
    if (!g_palette_tint) {
        // A neutral host frosts without a colour cast: the veil keeps the shipped
        // transparency and only the hue changes (no palette tint, no saturation
        // boost), so the pane stays as see-through as it always was.
        look.saturation = 1.0f;
    }
    if (options.tint.w > 0.0f) {
        look.tint = Mix(look.tint, ImVec4(options.tint.x, options.tint.y, options.tint.z, 1.0f),
            options.tint.w);
        look.tint.w = 1.0f;
    }
    if (options.tint_strength >= 0.0f) look.tint_strength = options.tint_strength;
    if (options.saturation >= 0.0f) look.saturation = options.saturation;
    if (options.minimum_alpha >= 0.0f) look.minimum_alpha = options.minimum_alpha;
    if (options.face_blur >= 0.0f) look.blur_mix = options.face_blur;
    if (options.rim_blur >= 0.0f) look.rim_blur = options.rim_blur;
    if (!options.rim) {
        look.rim = 0.0f;
        look.contact = 0.0f;
    }
    if (options.refraction_scale > 0.0f) look.refraction *= options.refraction_scale;
    if (!options.shadow) {
        look.shadow_alpha = 0.0f;
        look.contact = 0.0f;
    }
    return look;
}

ImU32 Color(const ImVec4& color, const float alpha) noexcept {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, color.w * alpha));
}

void DrawFallback(ImDrawList* list, const ImVec2 min, const ImVec2 max,
    const SurfaceOptions& options, const Look& look) noexcept {
    const float alpha = ImGui::GetStyle().Alpha;
    const float scale = options.scale;
    const float rounding = (std::min)(options.rounding * 1.0f,
        (std::min)(max.x - min.x, max.y - min.y) * 0.5f);
    if (!options.clip) list->PushClipRect(ImVec2(-16384.0f, -16384.0f),
        ImVec2(16384.0f, 16384.0f), false);
    if (look.shadow_alpha > 0.0f) {
        constexpr int kLayers = 5;
        for (int layer = 0; layer < kLayers; ++layer) {
            const float grow = look.shadow_blur * scale *
                static_cast<float>(layer + 1) / static_cast<float>(kLayers) * 0.6f;
            const float layer_alpha = look.shadow_alpha * 0.30f *
                (1.0f - static_cast<float>(layer) / static_cast<float>(kLayers));
            list->AddRectFilled(
                ImVec2(min.x - grow, min.y - grow + look.shadow_offset * scale),
                ImVec2(max.x + grow, max.y + grow + look.shadow_offset * scale),
                Color(ImVec4(0.0f, 0.0f, 0.0f, 1.0f), layer_alpha * alpha), rounding + grow);
        }
    }
    const float fill = g_reduce_transparency ? 0.98f : look.fallback_alpha;
    list->AddRectFilled(min, max, Color(look.tint, fill * alpha), rounding);
    const bool dark = DarkAppearance();
    list->AddRect(ImVec2(min.x + 0.5f, min.y + 0.5f), ImVec2(max.x - 0.5f, max.y - 0.5f),
        Color(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), (dark ? 0.12f : 0.70f) * look.rim * alpha),
        rounding, 0, (std::max)(1.0f, scale));
    if (!options.clip) list->PopClipRect();
}

}  // namespace

bool InitializeRenderer(ID3D11Device* device, ID3D11DeviceContext* context,
    const bool palette_tint) noexcept {
    ShutdownRenderer();
    if (device == nullptr || context == nullptr) return false;
    try {
        auto renderer = std::make_unique<Renderer>();
        renderer->device = device;
        renderer->context = context;
        g_palette_tint = palette_tint;
        if (FAILED(device->CreateVertexShader(kGlassFullScreenVs, sizeof(kGlassFullScreenVs),
                nullptr, &renderer->full_screen)) ||
            FAILED(device->CreateVertexShader(kGlassSurfaceVs, sizeof(kGlassSurfaceVs),
                nullptr, &renderer->quad)) ||
            FAILED(device->CreatePixelShader(kGlassDownsamplePs, sizeof(kGlassDownsamplePs),
                nullptr, &renderer->downsample)) ||
            FAILED(device->CreatePixelShader(kGlassBlurPs, sizeof(kGlassBlurPs),
                nullptr, &renderer->blur)) ||
            FAILED(device->CreatePixelShader(kGlassSurfacePs, sizeof(kGlassSurfacePs),
                nullptr, &renderer->glass))) {
            return false;
        }
        D3D11_BUFFER_DESC buffer{};
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        buffer.ByteWidth = sizeof(GlassConstants);
        if (FAILED(device->CreateBuffer(&buffer, nullptr, &renderer->glass_constants))) return false;
        buffer.ByteWidth = sizeof(PassConstants);
        if (FAILED(device->CreateBuffer(&buffer, nullptr, &renderer->pass_constants))) return false;

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&sampler, &renderer->sampler))) return false;

        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateBlendState(&blend, &renderer->opaque))) return false;
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        if (FAILED(device->CreateBlendState(&blend, &renderer->blend))) return false;

        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        raster.ScissorEnable = TRUE;
        if (FAILED(device->CreateRasterizerState(&raster, &renderer->pass_rasterizer))) return false;

        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        if (FAILED(device->CreateDepthStencilState(&depth, &renderer->depth_off))) return false;
        g_renderer = std::move(renderer);
        return true;
    } catch (...) {
        return false;
    }
}


bool InitializeRendererD3D12(ID3D12Device* const device, const DXGI_FORMAT format,
    const bool palette_tint) noexcept {
    ShutdownRenderer();
    if (device == nullptr || format == DXGI_FORMAT_UNKNOWN) return false;
    try {
        auto renderer = std::make_unique<D3d12Renderer>();
        renderer->device = device;
        renderer->format = format;
        g_palette_tint = palette_tint;
        // Inline uniforms are owned by the recorded command list. Blur passes set only
        // their first two float4s, and each surface supplies its full material.
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.Num32BitValues = sizeof(GlassConstants) / sizeof(UINT);
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_DESCRIPTOR_RANGE texture_range{};
        texture_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        texture_range.NumDescriptors = 3;
        texture_range.BaseShaderRegister = 0;
        texture_range.RegisterSpace = 0;
        texture_range.OffsetInDescriptorsFromTableStart = 0;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[1].DescriptorTable.pDescriptorRanges = &texture_range;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.RegisterSpace = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC signature_description{};
        signature_description.NumParameters = 2;
        signature_description.pParameters = parameters;
        signature_description.NumStaticSamplers = 1;
        signature_description.pStaticSamplers = &sampler;
        signature_description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> signature_error;
        if (FAILED(D3D12SerializeRootSignature(&signature_description,
                D3D_ROOT_SIGNATURE_VERSION_1, &signature, &signature_error))) {
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(),
                signature->GetBufferSize(), IID_PPV_ARGS(&renderer->root_signature)))) {
            return false;
        }
        // Every enum is filled in explicitly: D3D12 validates them even where the
        // stage is disabled, and a zeroed field is not "no setting".
        const auto build_pipeline = [&](const void* const vertex, const std::size_t vertex_size,
            const void* const pixel, const std::size_t pixel_size,
            const bool blend) -> ComPtr<ID3D12PipelineState> {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
        description.pRootSignature = renderer->root_signature.Get();
        description.VS.pShaderBytecode = vertex;
        description.VS.BytecodeLength = vertex_size;
        description.PS.pShaderBytecode = pixel;
        description.PS.BytecodeLength = pixel_size;
        D3D12_RENDER_TARGET_BLEND_DESC& target = description.BlendState.RenderTarget[0];
        target.BlendEnable = blend ? TRUE : FALSE;
        target.LogicOpEnable = FALSE;
        target.SrcBlend = blend ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_ONE;
        target.DestBlend = blend ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ZERO;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlendAlpha = blend ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ZERO;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        description.BlendState.AlphaToCoverageEnable = FALSE;
        description.BlendState.IndependentBlendEnable = FALSE;
        description.SampleMask = UINT_MAX;
        description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        description.RasterizerState.FrontCounterClockwise = FALSE;
        description.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        description.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        description.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        description.RasterizerState.DepthClipEnable = TRUE;
        description.RasterizerState.MultisampleEnable = FALSE;
        description.RasterizerState.AntialiasedLineEnable = FALSE;
        description.RasterizerState.ForcedSampleCount = 0;
        description.RasterizerState.ConservativeRaster =
            D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        D3D12_DEPTH_STENCIL_DESC& depth = description.DepthStencilState;
        depth.DepthEnable = FALSE;
        depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        depth.StencilEnable = FALSE;
        depth.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        depth.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        depth.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        depth.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        depth.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
        depth.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        depth.BackFace = depth.FrontFace;
        description.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        description.NumRenderTargets = 1;
        description.RTVFormats[0] = format;
        description.DSVFormat = DXGI_FORMAT_UNKNOWN;
        description.SampleDesc.Count = 1;
        description.SampleDesc.Quality = 0;
        description.NodeMask = 0;
        description.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
        ComPtr<ID3D12PipelineState> pipeline;
        if (FAILED(device->CreateGraphicsPipelineState(
                &description, IID_PPV_ARGS(&pipeline)))) {
            return {};
        }
        return pipeline;
        };
        renderer->downsample = build_pipeline(kGlassFullScreenVs, sizeof(kGlassFullScreenVs),
            kGlassDownsamplePs, sizeof(kGlassDownsamplePs), false);
        renderer->blur = build_pipeline(kGlassFullScreenVs, sizeof(kGlassFullScreenVs),
            kGlassBlurPs, sizeof(kGlassBlurPs), false);
        renderer->glass = build_pipeline(kGlassSurfaceVs, sizeof(kGlassSurfaceVs),
            kGlassSurfacePs, sizeof(kGlassSurfacePs), true);
        if (renderer->downsample == nullptr || renderer->blur == nullptr ||
            renderer->glass == nullptr) {
            return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
        heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_description.NumDescriptors = 5;
        if (FAILED(device->CreateDescriptorHeap(
                &heap_description, IID_PPV_ARGS(&renderer->rtv_heap)))) {
            return false;
        }
        renderer->rtv_stride =
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_DESCRIPTOR_HEAP_DESC srv_description{};
        srv_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_description.NumDescriptors = static_cast<UINT>(kGlassDescriptorCount);
        srv_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(
                &srv_description, IID_PPV_ARGS(&renderer->srv_heap)))) {
            return false;
        }
        renderer->srv_stride =
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        renderer->srv_base = renderer->srv_heap->GetGPUDescriptorHandleForHeapStart();
        renderer->surface_table.ptr = renderer->srv_base.ptr +
            static_cast<UINT64>(renderer->srv_stride) * kGlassSurfaceFirst;
        g_d3d12 = std::move(renderer);
        return true;
    } catch (...) {
        return false;
    }
}

void BeginD3D12Frame(ID3D12GraphicsCommandList* const list,
    ID3D12Resource* const back_buffer,
    const D3D12_CPU_DESCRIPTOR_HANDLE frame_rtv,
    ID3D12DescriptorHeap* const host_heap) noexcept {
    D3d12Renderer* const renderer = g_d3d12.get();
    if (renderer == nullptr) return;
    renderer->list = list;
    renderer->back_buffer = back_buffer;
    renderer->frame_rtv = frame_rtv;
    renderer->host_heap = host_heap;
    renderer->captured_frame = -1;
}
void ShutdownRenderer() noexcept {
    g_d3d12.reset();
    g_renderer.reset();
    g_commands.clear();
    g_commands_frame = -1;
    g_window_surfaces.clear();
    g_underlays.clear();
}

bool RendererAvailable() noexcept { return g_renderer != nullptr || g_d3d12 != nullptr; }

bool Active() noexcept { return RendererAvailable() && !g_reduce_transparency; }

void SetReduceTransparency(const bool enabled) noexcept { g_reduce_transparency = enabled; }

bool ReduceTransparency() noexcept { return g_reduce_transparency; }

void SetReduceMotion(const bool enabled) noexcept { g_reduce_motion = enabled; }

bool ReduceMotion() noexcept { return g_reduce_motion; }

bool DarkAppearance() noexcept {
    const auto surface = ue5mem::PlatformUiSurfaceColor();
    return 0.2126f * surface.red + 0.7152f * surface.green + 0.0722f * surface.blue < 0.45f;
}

void CaptureBackdrop(ImDrawList* list, const bool blur, const bool deep) noexcept {
    if (list == nullptr || !Active()) return;
    try {
        Command& command = NewCommand();
        command.kind = CommandKind::Capture;
        command.blur = blur;
        command.deep = blur && deep;
        list->AddCallback(CaptureCallback, &command);
        list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    } catch (...) {
    }
}

namespace {

// Records one surface over `min`..`max`; `sector` and `ring` shape it as a ring
// sector when the aperture in sector[3] is positive.
void RecordSurface(ImDrawList* list, const ImVec2 min, const ImVec2 max,
    const SurfaceOptions& options, const Look& look, const float (&sector)[4],
    const float (&ring)[4]) noexcept {
    try {
        Command& command = NewCommand();
        command.kind = CommandKind::Surface;
        command.clip = options.clip;
        GlassConstants& constants = command.constants;
        std::copy(std::begin(sector), std::end(sector), std::begin(constants.sector));
        std::copy(std::begin(ring), std::end(ring), std::begin(constants.ring));
        const float scale = options.scale > 0.0f ? options.scale : 1.0f;
        const float margin = look.shadow_alpha > 0.0f
            ? (look.shadow_blur + look.shadow_offset) * scale + 2.0f : 2.0f * scale;
        constants.rect[0] = min.x;
        constants.rect[1] = min.y;
        constants.rect[2] = max.x;
        constants.rect[3] = max.y;
        constants.shape[0] = options.rounding;
        constants.shape[1] = scale;
        constants.shape[2] = margin;
        constants.shape[3] = ImGui::GetStyle().Alpha;
        constants.tint[0] = look.tint.x;
        constants.tint[1] = look.tint.y;
        constants.tint[2] = look.tint.z;
        constants.tint[3] = look.tint_strength;
        constants.optics[0] = look.refraction;
        constants.optics[1] = look.chroma;
        constants.optics[2] = look.blur_mix;
        constants.optics[3] = look.saturation;
        constants.light[0] = look.rim;
        constants.light[1] = look.lift;
        constants.light[2] = DarkAppearance() ? 1.0f : 0.0f;
        constants.light[3] = look.minimum_alpha;
        constants.shadow[0] = look.shadow_alpha;
        constants.shadow[1] = look.shadow_offset * scale;
        constants.shadow[2] = look.shadow_blur * scale;
        constants.shadow[3] = look.gloss;
        constants.extra[0] = look.bevel;
        constants.extra[1] = look.glow;
        constants.extra[2] = look.magnify;
        constants.extra[3] = look.contact;
        constants.edge[0] = look.shade;
        constants.edge[1] = look.rim_blur;
        constants.edge[2] = (std::max)(0.0f, options.edge_feather) * scale;
        list->AddCallback(SurfaceCallback, &command);
        list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    } catch (...) {
    }
}

}  // namespace

void DrawSurface(ImDrawList* list, const ImVec2 min, const ImVec2 max,
    const SurfaceOptions& options) noexcept {
    if (list == nullptr || max.x - min.x < 1.0f || max.y - min.y < 1.0f) return;
    const Look look = ResolveLook(options);
    if (!Active()) {
        DrawFallback(list, min, max, options, look);
        return;
    }
    RecordSurface(list, min, max, options, look, {}, {});
}

void DrawSectorSurface(ImDrawList* list, const ImVec2 center, const float inner_radius,
    const float outer_radius, const float angle_start, const float angle_end,
    const SurfaceOptions& options) noexcept {
    constexpr float kPi = 3.14159265358979323846f;
    const float span = angle_end - angle_start;
    if (list == nullptr || !(inner_radius >= 0.0f) || !(outer_radius - inner_radius >= 1.0f) ||
        !(span > 0.0f) || !(span < kPi)) {
        return;
    }
    const Look look = ResolveLook(options);
    if (!Active()) {
        const int segments = (std::max)(4, static_cast<int>(span * outer_radius / 6.0f));
        if (!options.clip) list->PushClipRect(ImVec2(-16384.0f, -16384.0f),
            ImVec2(16384.0f, 16384.0f), false);
        list->PathArcTo(center, outer_radius, angle_start, angle_end, segments);
        list->PathArcTo(center, inner_radius, angle_end, angle_start, segments);
        const float fill = g_reduce_transparency ? 0.98f : look.fallback_alpha;
        list->PathFillConcave(Color(look.tint, fill * ImGui::GetStyle().Alpha));
        if (!options.clip) list->PopClipRect();
        return;
    }
    // The quad only has to cover the shape: bound both arcs, including any
    // axis extreme the span crosses.
    ImVec2 min(FLT_MAX, FLT_MAX);
    ImVec2 max(-FLT_MAX, -FLT_MAX);
    const auto include = [&](const float radius, const float angle) {
        const ImVec2 point(center.x + radius * std::cos(angle), center.y + radius * std::sin(angle));
        min = ImVec2((std::min)(min.x, point.x), (std::min)(min.y, point.y));
        max = ImVec2((std::max)(max.x, point.x), (std::max)(max.y, point.y));
    };
    include(inner_radius, angle_start);
    include(inner_radius, angle_end);
    include(outer_radius, angle_start);
    include(outer_radius, angle_end);
    for (int quarter = -8; quarter <= 8; ++quarter) {
        const float angle = static_cast<float>(quarter) * kPi * 0.5f;
        if (angle > angle_start && angle < angle_end) include(outer_radius, angle);
    }
    const float sector[4]{center.x, center.y, (angle_start + angle_end) * 0.5f, span * 0.5f};
    const float ring[4]{inner_radius, outer_radius, 0.0f, 0.0f};
    RecordSurface(list, min, max, options, look, sector, ring);
}

void RegisterWindowSurface(const SurfaceOptions& options) noexcept {
    ImGuiWindow* const window = ImGui::GetCurrentWindow();
    if (window == nullptr) return;
    ImGuiWindow* const root = window->RootWindow != nullptr ? window->RootWindow : window;
    try {
        g_window_surfaces[root->ID] = {ImGui::GetFrameCount(), options};
    } catch (...) {
    }
}

namespace {

void BoundFrameCaptures(const ImDrawData& data) noexcept {
    // Both glass backends use framebuffer pixels. Other viewport transforms retain full captures.
    if (data.DisplayPos.x != 0 || data.DisplayPos.y != 0 ||
        data.FramebufferScale.x != 1 || data.FramebufferScale.y != 1) return;
    const ImRect screen(ImVec2(0, 0), data.DisplaySize);
    const ImRect empty(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
    ImRect sharp = empty, blurred = empty;
    for (int i = data.CmdLists.Size - 1; i >= 0; --i) {
        const auto& draws = data.CmdLists[i]->CmdBuffer;
        for (int j = draws.Size - 1; j >= 0; --j) {
            const ImDrawCmd& draw = draws[j];
            if (draw.UserCallback == CaptureCallback) {
                auto& capture = *static_cast<Command*>(draw.UserCallbackData);
                capture.bounded_copy = true;
                capture.sampled_region = sharp;
                sharp = empty;
                // A thumb replaces just the sharp copy. Later frosted controls can still
                // read the half and quarter textures produced by the preceding blur.
                if (capture.blur) {
                    capture.blurred_region = blurred;
                    blurred = empty;
                }
            } else if (draw.UserCallback == SurfaceCallback) {
                const auto& c = static_cast<const Command*>(draw.UserCallbackData)->constants;
                // Magnification pulls towards a point inside the lens bounds, including for
                // ring sectors. Add the feather, refracted RGB taps and their linear footprint.
                const float reach = (std::max)(1.0f, c.edge[2]) * 0.5f + 1.0f +
                    std::abs(c.optics[0]) * c.shape[1] * (1.0f + std::abs(c.optics[1]));
                ImRect lens(c.rect[0] - reach, c.rect[1] - reach, c.rect[2] + reach, c.rect[3] + reach);
                lens.ClipWith(screen);
                if (lens.GetWidth() <= 0 || lens.GetHeight() <= 0) continue;
                sharp.Add(lens);
                if (c.optics[2] > 0.0f) blurred.Add(lens);
            } else if (draw.UserCallback && draw.UserCallback != ImDrawCallback_ResetRenderState) {
                sharp = blurred = screen;
            }
        }
    }
}

} // namespace

void PrepareDrawData(ImDrawData* data, const bool retain_desktop_layer) noexcept {
    if (data == nullptr || ImGui::GetCurrentContext() == nullptr || !Active()) return;
    try {
        ImGuiContext& context = *GImGui;
        const int frame = ImGui::GetFrameCount();
        std::size_t used = 0;
        for (ImGuiWindow* const window : context.Windows) {
            if (window == nullptr || !window->Active || window->Hidden ||
                window->DrawList == nullptr) {
                continue;
            }
            if ((window->Flags & ImGuiWindowFlags_ChildWindow) != 0 &&
                (window->Flags & ImGuiWindowFlags_Popup) == 0) {
                continue;
            }
            SurfaceOptions options;
            const auto registered = g_window_surfaces.find(window->ID);
            if (registered != g_window_surfaces.end() && registered->second.frame == frame) {
                options = registered->second.options;
            } else if ((window->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip)) != 0) {
                options.rounding = (window->Flags & ImGuiWindowFlags_Tooltip) != 0
                    ? context.Style.PopupRounding * 0.75f : context.Style.PopupRounding;
                options.scale = ImGui::GetIO().FontGlobalScale > 0.0f
                    ? ImGui::GetIO().FontGlobalScale : 1.0f;
                options.clip = false;
            } else {
                continue;
            }
            const int index = data->CmdLists.find_index(window->DrawList);
            if (index < 0) continue;
            if (used == g_underlays.size()) {
                g_underlays.push_back(
                    std::make_unique<ImDrawList>(ImGui::GetDrawListSharedData()));
            }
            ImDrawList* const underlay = g_underlays[used++].get();
            underlay->_ResetForNewFrame();
            underlay->PushClipRectFullScreen();
            underlay->PushTextureID(ImGui::GetIO().Fonts->TexID);
            CaptureBackdrop(underlay, true, options.deep_backdrop);
            DrawSurface(underlay, window->Pos, window->Rect().Max, options);
            underlay->_PopUnusedDrawCmd();
            data->CmdLists.insert(data->CmdLists.Data + index, underlay);
            data->CmdListsCount = data->CmdLists.Size;
            data->TotalVtxCount += underlay->VtxBuffer.Size;
            data->TotalIdxCount += underlay->IdxBuffer.Size;
        }
        std::erase_if(g_window_surfaces,
            [frame](const auto& entry) { return entry.second.frame < frame - 1; });
        if (!retain_desktop_layer && g_commands_frame == frame) BoundFrameCaptures(*data);
    } catch (...) {
    }
}

bool AppendDrawListSnapshot(const ImDrawList& list, std::vector<std::byte>& bytes) {
    const auto append = [&bytes](auto values) {
        const auto data = std::as_bytes(values);
        if (!data.empty()) bytes.insert(bytes.end(), data.begin(), data.end());
    };
    const int sizes[]{list.VtxBuffer.Size, list.IdxBuffer.Size, list.CmdBuffer.Size};
    append(std::span{sizes});
    append(std::span{list.VtxBuffer.Data, static_cast<std::size_t>(list.VtxBuffer.Size)});
    append(std::span{list.IdxBuffer.Data, static_cast<std::size_t>(list.IdxBuffer.Size)});
    for (const ImDrawCmd& draw : list.CmdBuffer) {
        // ImGui zero-initializes the command header. It contains the clip, texture, offsets,
        // element count and callback identity, ending just before its transient data pointer.
        append(std::as_bytes(std::span{&draw, 1}).first(offsetof(ImDrawCmd, UserCallbackData)));
        if (draw.UserCallback == nullptr || draw.UserCallback == ImDrawCallback_ResetRenderState) continue;
        if (draw.UserCallback != CaptureCallback && draw.UserCallback != SurfaceCallback) return false;
        const auto& command = *static_cast<const Command*>(draw.UserCallbackData);
        if (draw.UserCallback == CaptureCallback) {
            const bool flags[]{command.blur, command.deep};
            append(std::span{flags});
        } else {
            append(std::span{&command.clip, 1});
            append(std::span{&command.constants, 1});
        }
    }
    return true;
}

bool SaveDesktopLayer(ID3D11RenderTargetView* target) noexcept {
    Renderer* const renderer = g_renderer.get();
    if (!renderer || !target || renderer->captured_frame != ImGui::GetFrameCount()) return false;
    ComPtr<ID3D11Resource> color;
    target->GetResource(&color);
    const std::array<ID3D11Resource*, 4> inputs{
        color.Get(), renderer->backdrop.Get(), renderer->half.texture.Get(), renderer->quarter.texture.Get()};
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        D3D11_TEXTURE2D_DESC desc{};
        static_cast<ID3D11Texture2D*>(inputs[i])->GetDesc(&desc);
        auto& saved = renderer->desktop_layer[i];
        D3D11_TEXTURE2D_DESC previous{};
        if (saved) saved->GetDesc(&previous);
        if (previous.Width != desc.Width || previous.Height != desc.Height || previous.Format != desc.Format) {
            desc.BindFlags = i == 0 ? 0 : D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = 0;
            saved.Reset();
            if (FAILED(renderer->device->CreateTexture2D(&desc, nullptr, &saved))) return false;
            if (i != 0 && FAILED(renderer->device->CreateShaderResourceView(
                    saved.Get(), nullptr, renderer->desktop_views[i - 1].ReleaseAndGetAddressOf()))) {
                saved.Reset();
                return false;
            }
        }
        renderer->context->CopyResource(saved.Get(), inputs[i]);
    }
    return true;
}

void RestoreDesktopLayer(ID3D11RenderTargetView* target, const RECT& area) noexcept {
    Renderer& renderer = *g_renderer;
    ComPtr<ID3D11Resource> color;
    target->GetResource(&color);
    const D3D11_BOX box{static_cast<UINT>(area.left), static_cast<UINT>(area.top), 0,
        static_cast<UINT>(area.right), static_cast<UINT>(area.bottom), 1};
    renderer.context->CopySubresourceRegion(color.Get(), 0, box.left, box.top, 0,
        renderer.desktop_layer[0].Get(), 0, &box);
    renderer.desktop_sharp = true;
    renderer.desktop_blur = true;
    renderer.captured_frame = ImGui::GetFrameCount();
}

ImRect BoundDesktopCaptures(const ImDrawData& data) noexcept {
    const ImRect screen(data.DisplayPos, ImVec2(data.DisplayPos.x + data.DisplaySize.x,
        data.DisplayPos.y + data.DisplaySize.y));
    ImRect bounds(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
    Command* capture = nullptr;
    for (const ImDrawList* list : data.CmdLists) {
        for (const ImDrawCmd& draw : list->CmdBuffer) {
            if (draw.UserCallback == CaptureCallback) {
                auto& next = *static_cast<Command*>(draw.UserCallbackData);
                next.bounded_copy = !next.blur;
                next.sampled_region = ImRect(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
                capture = next.blur ? nullptr : &next;
                if (next.blur) bounds = screen;
            } else if (draw.UserCallback == SurfaceCallback) {
                const auto& c = static_cast<const Command*>(draw.UserCallbackData)->constants;
                if (c.sector[3] > 0.0f) {
                    if (capture) capture->bounded_copy = false;
                    capture = nullptr;
                    bounds = screen;
                    continue;
                }
                // A rectangular lens magnifies toward its own centre. Only refraction and
                // chromatic separation can sample beyond the quad. Include the linear filter footprint.
                const float reach = c.shape[2] + std::abs(c.optics[0]) * c.shape[1] *
                    (1.0f + std::abs(c.optics[1])) + 1.0f;
                const ImRect lens(c.rect[0] - reach, c.rect[1] - reach,
                    c.rect[2] + reach, c.rect[3] + reach);
                if (capture) capture->sampled_region.Add(lens);
                bounds.Add(lens);
            } else if (draw.UserCallback == nullptr) {
                if (draw.ElemCount) bounds.Add(ImRect(draw.ClipRect));
            } else if (draw.UserCallback != ImDrawCallback_ResetRenderState) {
                if (capture) capture->bounded_copy = false;
                capture = nullptr;
                bounds = screen;
            }
        }
    }
    bounds.ClipWith(screen);
    return bounds;
}

void DrawThumb(ImDrawList* list, const ImVec2 min, const ImVec2 max, const float rounding,
    const float scale, const bool accent) noexcept {
    if (list == nullptr || max.x - min.x < 1.0f || max.y - min.y < 1.0f) return;
    const auto& theme = ue5mem::PlatformUiTheme();
    SurfaceOptions options;
    options.variant = Variant::Thumb;
    options.rounding = rounding;
    options.scale = scale;
    options.clip = true;
    if (accent) {
        options.tint = ImVec4(theme.accent.red, theme.accent.green, theme.accent.blue, 0.75f);
        options.tint_strength = DarkAppearance() ? 0.18f : 0.22f;
    }
    if (Active()) {
        // The lens refracts what is already drawn, its own label included.
        CaptureBackdrop(list, false);
        DrawSurface(list, min, max, options);
        return;
    }
    const ImVec4 fill = accent
        ? ImVec4(theme.accent.red, theme.accent.green, theme.accent.blue,
              g_reduce_transparency ? 0.34f : 0.26f)
        : ImVec4(1.0f, 1.0f, 1.0f, DarkAppearance() ? 0.16f : 0.80f);
    list->AddRectFilled(min, max, ImGui::GetColorU32(fill), rounding);
    list->AddRect(ImVec2(min.x + 0.5f, min.y + 0.5f), ImVec2(max.x - 0.5f, max.y - 0.5f),
        ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, DarkAppearance() ? 0.14f : 0.65f)),
        rounding, 0, (std::max)(1.0f, scale));
}

ImRect AnimateThumb(const ImGuiID id, const ImRect& target, const ImVec2 origin) noexcept {
    const ImVec2 target_center = target.GetCenter();
    const float x = Animate(id, target_center.x - origin.x, 18.0f);
    const float y = Animate(id ^ 0x632be5abU, target_center.y - origin.y, 18.0f);
    const float width = Animate(id ^ 0x85ebca6bU, target.GetWidth(), 18.0f);
    const float height = Animate(id ^ 0xc2b2ae35U, target.GetHeight(), 18.0f);
    const ImVec2 center(origin.x + x, origin.y + y);
    return ImRect(ImVec2(center.x - width * 0.5f, center.y - height * 0.5f),
        ImVec2(center.x + width * 0.5f, center.y + height * 0.5f));
}

float Animate(const ImGuiID id, const float target, const float speed) noexcept {
    ImGuiStorage* const storage = ImGui::GetStateStorage();
    if (storage == nullptr) return target;
    const ImGuiID key = ImHashStr("##glass-animation", 0, id);
    float* const value = storage->GetFloatRef(key, target);
    if (value == nullptr) return target;
    if (g_reduce_motion) {
        *value = target;
        return target;
    }
    const float delta = (std::max)(0.0f, ImGui::GetIO().DeltaTime);
    *value += (target - *value) * (1.0f - std::exp(-speed * delta));
    if (std::fabs(target - *value) < 0.002f) *value = target;
    return *value;
}

ImVec2 SwitchSize() noexcept {
    const float height = std::round(ImGui::GetFrameHeight() * 0.96f);
    return {std::round(height * 1.66f), height};
}

void DrawSwitch(ImDrawList* list, const ImVec2 min, const ImVec2 max, const float on,
    const float press, const bool hovered, const float scale) noexcept {
    if (list == nullptr) return;
    const auto& theme = ue5mem::PlatformUiTheme();
    const bool dark = DarkAppearance();
    const float height = max.y - min.y;
    const float width = max.x - min.x;
    const float radius = height * 0.5f;
    const ImVec4 accent = ControlAccent();
    const ImVec4 off = dark ? ImVec4(1.0f, 1.0f, 1.0f, hovered ? 0.22f : 0.17f)
                            : ImVec4(0.47f, 0.47f, 0.50f, hovered ? 0.30f : 0.24f);
    const ImVec4 track = Mix(off, ImVec4(accent.x, accent.y, accent.z, 1.0f), on);
    list->AddRectFilled(min, max, ImGui::GetColorU32(track), radius);
    // A light accent needs an edge of its own to read against a light card.
    const float accent_luminance = 0.2126f * accent.x + 0.7152f * accent.y + 0.0722f * accent.z;
    if (!dark && accent_luminance > 0.55f && on > 0.0f) {
        const ImVec4 edge = Mix(ImVec4(accent.x, accent.y, accent.z, 1.0f),
            ImVec4(0.0f, 0.0f, 0.0f, 1.0f), 0.30f);
        list->AddRect(ImVec2(min.x + 0.5f, min.y + 0.5f), ImVec2(max.x - 0.5f, max.y - 0.5f),
            ImGui::GetColorU32(ImVec4(edge.x, edge.y, edge.z, 0.55f * on)), radius, 0,
            (std::max)(1.0f, scale));
    }

    const float inset = (std::max)(2.0f, std::round(height * 0.09f));
    const float knob_radius = radius - inset;
    const float stretch = press * height * 0.30f;
    const float travel = width - inset * 2.0f - knob_radius * 2.0f - stretch;
    const float knob_left = min.x + inset + travel * on;
    const ImVec2 knob_min(knob_left, min.y + inset);
    const ImVec2 knob_max(knob_left + knob_radius * 2.0f + stretch, max.y - inset);
    if (press > 0.02f && Active()) {
        // While the switch is held its knob becomes a clear lens over the track,
        // the one place Apple puts glass into the content layer. The lens only
        // reads the sharp copy, so it takes the cheap capture: a second blurred
        // chain per frame bought nothing here and cost every frame the switch
        // was held, which is exactly when a dropped frame shows.
        CaptureBackdrop(list, false);
        const float grow = press * height * 0.16f;
        SurfaceOptions lens;
        lens.variant = Variant::Clear;
        lens.rounding = knob_radius + grow;
        lens.scale = scale;
        lens.clip = false;
        DrawSurface(list, ImVec2(knob_min.x - grow, knob_min.y - grow),
            ImVec2(knob_max.x + grow, knob_max.y + grow), lens);
    }
    const float solid = Active() ? 1.0f - press : 1.0f;
    if (solid > 0.0f) {
        for (int layer = 0; layer < 3; ++layer) {
            const float grow = static_cast<float>(layer) * 0.8f * scale;
            list->AddRectFilled(ImVec2(knob_min.x - grow, knob_min.y - grow + scale),
                ImVec2(knob_max.x + grow, knob_max.y + grow + 1.5f * scale),
                ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.07f * solid)),
                knob_radius + grow);
        }
        list->AddRectFilled(knob_min, knob_max,
            ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, solid)), knob_radius);
    }
}

bool Switch(const char* label, bool* value) noexcept {
    ImGuiWindow* const window = ImGui::GetCurrentWindow();
    if (window == nullptr || window->SkipItems || value == nullptr) return false;
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiID id = window->GetID(label);
    const ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 size = SwitchSize();
    const float height = (std::max)(ImGui::GetFrameHeight(), size.y);
    const ImVec2 position = window->DC.CursorPos;
    const ImRect bounds(position, ImVec2(position.x + size.x +
        (label_size.x > 0.0f ? style.ItemInnerSpacing.x + label_size.x : 0.0f),
        position.y + height));
    ImGui::ItemSize(bounds, style.FramePadding.y);
    if (!ImGui::ItemAdd(bounds, id)) return false;
    bool hovered{};
    bool held{};
    const bool pressed = ImGui::ButtonBehavior(bounds, id, &hovered, &held);
    if (pressed) {
        *value = !*value;
        ImGui::MarkItemEdited(id);
    }
    const float top = position.y + (height - size.y) * 0.5f;
    const ImRect track(ImVec2(position.x, top), ImVec2(position.x + size.x, top + size.y));
    const float on = Animate(id, *value ? 1.0f : 0.0f, 15.0f);
    const float press = Animate(id ^ 0x5bd1e995U, held && hovered ? 1.0f : 0.0f, 22.0f);
    ImGui::RenderNavCursor(track, id);
    const float scale = ImGui::GetIO().FontGlobalScale > 0.0f ? ImGui::GetIO().FontGlobalScale : 1.0f;
    DrawSwitch(window->DrawList, track.Min, track.Max, on, press, hovered, scale);
    if (label_size.x > 0.0f) {
        ImGui::RenderText(ImVec2(track.Max.x + style.ItemInnerSpacing.x,
            position.y + (height - label_size.y) * 0.5f), label);
    }
    return pressed;
}

float SegmentedControlWidth(
    const std::span<const char* const> labels, const SegmentedOptions& options) noexcept {
    const float scale = options.scale;
    float width = 3.0f * scale * 2.0f;
    for (const char* const label : labels) {
        width += ImGui::CalcTextSize(label, nullptr, true).x + options.padding_x * scale * 2.0f;
    }
    return width;
}

// Corner radius as a fraction of height. Apple's controls use continuous
// rounded rectangles; a full capsule reads as exaggerated at these sizes.
constexpr float kSegmentRounding = 0.34f;

bool SegmentedControl(const char* id, const std::span<const char* const> labels, int& selected,
    const SegmentedOptions& options) noexcept {
    ImGuiWindow* const window = ImGui::GetCurrentWindow();
    if (window == nullptr || window->SkipItems || labels.empty()) return false;
    const float scale = options.scale;
    const float height = options.height * scale;
    const float inset = 3.0f * scale;
    const float padding = options.padding_x * scale;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = SegmentedControlWidth(labels, options);
    ImDrawList* const list = window->DrawList;
    const bool dark = DarkAppearance();
    if (options.glass_track) {
        SurfaceOptions track;
        track.variant = Variant::Regular;
        track.rounding = height * kSegmentRounding;
        track.scale = scale;
        track.shadow = false;
        if (g_palette_tint) track.tint_strength = DarkAppearance() ? 0.30f : 0.20f;
        DrawSurface(list, origin, ImVec2(origin.x + width, origin.y + height), track);
    } else {
        list->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
            ImGui::GetColorU32(dark ? ImVec4(1.0f, 1.0f, 1.0f, 0.07f)
                                    : ImVec4(0.47f, 0.47f, 0.50f, 0.13f)),
            height * kSegmentRounding);
    }

    ImGui::PushID(id);
    const ImGuiID base = window->GetID("##segmented");
    std::vector<float> starts(labels.size());
    std::vector<float> widths(labels.size());
    float x = origin.x + inset;
    for (std::size_t index = 0; index < labels.size(); ++index) {
        starts[index] = x;
        widths[index] = ImGui::CalcTextSize(labels[index], nullptr, true).x + padding * 2.0f;
        x += widths[index];
    }
    const int clamped = std::clamp(selected, 0, static_cast<int>(labels.size()) - 1);
    const std::size_t current_index = static_cast<std::size_t>(clamped);
    const float indicator_rounding = (height - inset * 2.0f) * kSegmentRounding;
    {
        // The lens goes down first so the labels stay crisp on top of it. It
        // swells a little while the control is held, like a pressed droplet.
        const bool held = ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsWindowHovered() &&
            ImGui::IsMouseHoveringRect(origin, ImVec2(origin.x + width, origin.y + height));
        const float press = Animate(base ^ 0x1b873593U, held ? 1.0f : 0.0f, 20.0f);
        const ImRect target(ImVec2(starts[current_index], origin.y + inset),
            ImVec2(starts[current_index] + widths[current_index], origin.y + height - inset));
        ImRect thumb = AnimateThumb(base, target, origin);
        const float grow = press * 2.0f * scale;
        thumb.Expand(ImVec2(grow * 1.5f, grow));
        DrawThumb(list, thumb.Min, thumb.Max, indicator_rounding + grow, scale, true);
    }
    bool changed = false;
    const ImVec2 cursor = ImGui::GetCursorPos();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    for (std::size_t index = 0; index < labels.size(); ++index) {
        const bool disabled = index < 32 && (options.disabled_mask & (1U << index)) != 0;
        ImGui::SetCursorScreenPos(ImVec2(starts[index], origin.y + inset));
        ImGui::PushID(static_cast<int>(index));
        ImGui::BeginDisabled(disabled);
        const bool clicked = ImGui::InvisibleButton(
            "##segment", ImVec2(widths[index], height - inset * 2.0f));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::EndDisabled();
        ImGui::PopID();
        if (clicked && !disabled &&
            (selected != static_cast<int>(index) || options.activate_selected)) {
            selected = static_cast<int>(index);
            changed = true;
        }
        const char* const label = labels[index];
        const char* const label_end = ImGui::FindRenderedTextEnd(label);
        const ImVec2 label_size = ImGui::CalcTextSize(label, label_end);
        const bool current = static_cast<int>(index) == clamped;
        ImVec4 ink = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ink.w *= disabled ? 0.35f : current ? 1.0f : hovered ? 0.92f : 0.72f;
        list->AddText(ImVec2(starts[index] + (widths[index] - label_size.x) * 0.5f,
                origin.y + (height - label_size.y) * 0.5f),
            ImGui::GetColorU32(ink), label, label_end);
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    ImGui::SetCursorPos(cursor);
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, height));
    return changed;
}

namespace {

constexpr int kTabBarStyleColors = 6;
constexpr int kTabBarStyleVars = 4;

void PushTabBarStyle() noexcept {
    const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Tab, clear);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, clear);
    ImGui::PushStyleColor(ImGuiCol_TabSelected, clear);
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, clear);
    ImGui::PushStyleColor(ImGuiCol_TabDimmed, clear);
    ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, clear);
    const float height = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, height * kSegmentRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_TabBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_TabBarBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_TabBarOverlineSize, 0.0f);
}

void PopTabBarStyle() noexcept {
    ImGui::PopStyleVar(kTabBarStyleVars);
    ImGui::PopStyleColor(kTabBarStyleColors);
}

float CurrentScale() noexcept {
    const float scale = ImGui::GetIO().FontGlobalScale;
    return scale > 0.0f ? scale : 1.0f;
}

}  // namespace

bool BeginTabBar(const char* id, const ImGuiTabBarFlags flags) noexcept {
    ImGuiWindow* const window = ImGui::GetCurrentWindow();
    if (window == nullptr || window->SkipItems) return false;
    const float scale = CurrentScale();
    const float inset = 3.0f * scale;
    // The bar sits inside a glass capsule as wide as its tabs were last frame;
    // the first frame of a new bar has no width yet and simply skips the track.
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + inset, ImGui::GetCursorPosY() + inset));
    const ImGuiID bar_id = window->GetID(id);
    const ImGuiTabBar* const previous = GImGui->TabBars.GetByKey(bar_id);
    const ImVec2 origin = window->DC.CursorPos;
    const float height = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f;
    if (previous != nullptr && previous->WidthAllTabs > 0.0f) {
        const float available = window->WorkRect.Max.x - origin.x;
        const float width = (std::min)(previous->WidthAllTabs, available);
        // Plugin windows have their own glass body, so the track refracts that.
        CaptureBackdrop(window->DrawList);
        SurfaceOptions track;
        track.variant = Variant::Regular;
        track.rounding = (height + inset * 2.0f) * kSegmentRounding;
        track.scale = scale;
        track.shadow = false;
        if (g_palette_tint) track.tint_strength = DarkAppearance() ? 0.30f : 0.20f;
        DrawSurface(window->DrawList, ImVec2(origin.x - inset, origin.y - inset),
            ImVec2(origin.x + width + inset, origin.y + height + inset), track);
        for (const ImGuiTabItem& tab : previous->Tabs) {
            if (tab.ID != previous->SelectedTabId || tab.Width <= 0.0f) continue;
            const float left = origin.x + tab.Offset - previous->ScrollingAnim;
            const ImRect target(ImVec2(left, origin.y), ImVec2(left + tab.Width, origin.y + height));
            const ImRect thumb = AnimateThumb(bar_id ^ 0x7f4a7c15U, target, origin);
            window->DrawList->PushClipRect(ImVec2(origin.x - inset, origin.y - inset),
                ImVec2(origin.x + width + inset, origin.y + height + inset), true);
            DrawThumb(window->DrawList, thumb.Min, thumb.Max, height * kSegmentRounding,
                scale, true);
            window->DrawList->PopClipRect();
            break;
        }
    }
    PushTabBarStyle();
    if (!ImGui::BeginTabBar(id, flags)) {
        PopTabBarStyle();
        return false;
    }
    return true;
}

void EndTabBar() noexcept {
    ImGui::EndTabBar();
    PopTabBarStyle();
    ImGui::Dummy(ImVec2(0.0f, 3.0f * CurrentScale()));
}

ImVec2 GlyphInkCenter(ImFont* font, const float size, const char* glyph) noexcept {
    if (font == nullptr || glyph == nullptr || *glyph == '\0') return ImVec2(0.0f, 0.0f);
    unsigned int codepoint = 0;
    ImTextCharFromUtf8(&codepoint, glyph, nullptr);
    const ImFontGlyph* const found = font->FindGlyphNoFallback(static_cast<ImWchar>(codepoint));
    if (found == nullptr || font->FontSize <= 0.0f) {
        const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, glyph);
        return ImVec2(extent.x * 0.5f, extent.y * 0.5f);
    }
    const float k = size / font->FontSize;
    return ImVec2((found->X0 + found->X1) * 0.5f * k, (found->Y0 + found->Y1) * 0.5f * k);
}

bool SliderScalar(const char* label, const ImGuiDataType data_type, void* value,
    const void* minimum, const void* maximum, const char* format,
    const ImGuiSliderFlags flags) noexcept {
    ImGuiWindow* const window = ImGui::GetCurrentWindow();
    if (window == nullptr || window->SkipItems || value == nullptr) return false;
    ImGuiContext& context = *GImGui;
    const ImGuiStyle& style = context.Style;
    const ImGuiID id = window->GetID(label);
    const float width = ImGui::CalcItemWidth();
    const ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
    const float height = ImGui::GetFrameHeight();
    const ImVec2 position = window->DC.CursorPos;
    const ImRect frame(position, ImVec2(position.x + width, position.y + height));
    const ImRect total(frame.Min, ImVec2(frame.Max.x +
        (label_size.x > 0.0f ? style.ItemInnerSpacing.x + label_size.x : 0.0f), frame.Max.y));
    const bool input_allowed = (flags & ImGuiSliderFlags_NoInput) == 0;
    ImGui::ItemSize(total, style.FramePadding.y);
    if (!ImGui::ItemAdd(total, id, &frame, input_allowed ? ImGuiItemFlags_Inputable : 0)) {
        return false;
    }
    if (format == nullptr) format = ImGui::DataTypeGetInfo(data_type)->PrintFmt;
    const bool hovered = ImGui::ItemHoverable(frame, id, context.LastItemData.ItemFlags);
    bool typing = input_allowed && ImGui::TempInputIsActive(id);
    if (!typing) {
        const bool clicked = hovered && ImGui::IsMouseClicked(0, ImGuiInputFlags_None, id);
        const bool activate = clicked || context.NavActivateId == id;
        if (activate && clicked) ImGui::SetKeyOwner(ImGuiKey_MouseLeft, id);
        if (activate && input_allowed &&
            ((clicked && context.IO.KeyCtrl) ||
                (context.NavActivateId == id &&
                    (context.NavActivateFlags & ImGuiActivateFlags_PreferInput) != 0))) {
            typing = true;
        }
        if (activate && !typing) {
            ImGui::SetActiveID(id, window);
            ImGui::SetFocusID(id, window);
            ImGui::FocusWindow(window);
            context.ActiveIdUsingNavDirMask |= (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
        }
    }
    if (typing) {
        const bool clamp = (flags & ImGuiSliderFlags_ClampOnInput) != 0;
        return ImGui::TempInputScalar(frame, id, label, data_type, value, format,
            clamp ? minimum : nullptr, clamp ? maximum : nullptr);
    }

    // The value column is as wide as the widest value the slider can show.
    char text[64];
    const auto format_value = [&] {
        return text + ImGui::DataTypeFormatString(text, IM_ARRAYSIZE(text), data_type, value, format);
    };
    const char* text_end = format_value();
    float value_width = ImGui::CalcTextSize(text, text_end).x;
    for (const void* bound : {minimum, maximum}) {
        char bound_text[64];
        const int length = ImGui::DataTypeFormatString(
            bound_text, IM_ARRAYSIZE(bound_text), data_type, bound, format);
        value_width = (std::max)(value_width, ImGui::CalcTextSize(bound_text, bound_text + length).x);
    }
    const float scale = CurrentScale();
    const float gap = 10.0f * scale;
    const float knob = std::round(height * 0.66f);
    const ImRect track(frame.Min, ImVec2((std::max)(frame.Min.x + knob * 2.0f,
        frame.Max.x - value_width - gap), frame.Max.y));

    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, knob);
    ImRect grab;
    const bool changed = ImGui::SliderBehavior(
        track, id, data_type, value, minimum, maximum, format, flags, &grab);
    ImGui::PopStyleVar();
    if (changed) {
        ImGui::MarkItemEdited(id);
        text_end = format_value();  // Draw the new value in the same frame as the thumb.
    }

    ImDrawList* const list = window->DrawList;
    const auto& theme = ue5mem::PlatformUiTheme();
    const bool dark = DarkAppearance();
    const bool active = context.ActiveId == id;
    ImGui::RenderNavCursor(frame, id);
    const float center_y = frame.GetCenter().y;
    const float rail = (std::max)(3.0f, std::round(4.0f * scale));
    const float knob_x = grab.Max.x > grab.Min.x ? grab.GetCenter().x : track.Min.x + knob * 0.5f;
    const ImVec2 rail_min(track.Min.x + knob * 0.5f - rail * 0.5f, center_y - rail * 0.5f);
    const ImVec2 rail_max(track.Max.x - knob * 0.5f + rail * 0.5f, center_y + rail * 0.5f);
    list->AddRectFilled(rail_min, rail_max, ImGui::GetColorU32(dark
        ? ImVec4(1.0f, 1.0f, 1.0f, 0.18f) : ImVec4(0.47f, 0.47f, 0.50f, 0.26f)), rail * 0.5f);
    static_cast<void>(theme);
    list->AddRectFilled(rail_min, ImVec2(knob_x, rail_max.y),
        ImGui::GetColorU32(ControlAccent()), rail * 0.5f);
    const float press = Animate(id ^ 0x2545f491U, active ? 1.0f : 0.0f, 22.0f);
    const ImVec2 knob_min(knob_x - knob * 0.5f, center_y - knob * 0.5f);
    const ImVec2 knob_max(knob_x + knob * 0.5f, center_y + knob * 0.5f);
    if (press > 0.02f && Active()) {
        CaptureBackdrop(list, false);
        const float grow = press * knob * 0.22f;
        SurfaceOptions lens;
        lens.variant = Variant::Clear;
        lens.rounding = knob * 0.5f + grow;
        lens.scale = scale;
        lens.clip = false;
        DrawSurface(list, ImVec2(knob_min.x - grow * 1.3f, knob_min.y - grow),
            ImVec2(knob_max.x + grow * 1.3f, knob_max.y + grow), lens);
    }
    const float solid = Active() ? 1.0f - press : 1.0f;
    if (solid > 0.0f) {
        for (int layer = 0; layer < 3; ++layer) {
            const float spread = static_cast<float>(layer) * 0.8f * scale;
            list->AddCircleFilled(ImVec2(knob_x, center_y + scale), knob * 0.5f + spread,
                ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.07f * solid)));
        }
        list->AddCircleFilled(ImVec2(knob_x, center_y), knob * 0.5f,
            ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, solid)));
    }
    const ImVec2 text_size = ImGui::CalcTextSize(text, text_end);
    ImVec4 ink = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (!hovered && !active) ink.w *= 0.72f;
    list->AddText(ImVec2(frame.Max.x - text_size.x, center_y - text_size.y * 0.5f),
        ImGui::GetColorU32(ink), text, text_end);
    if (label_size.x > 0.0f) {
        ImGui::RenderText(ImVec2(frame.Max.x + style.ItemInnerSpacing.x,
            frame.Min.y + style.FramePadding.y), label);
    }
    return changed;
}

bool SliderFloat(const char* label, float* value, const float minimum, const float maximum,
    const char* format, const ImGuiSliderFlags flags) noexcept {
    return SliderScalar(label, ImGuiDataType_Float, value, &minimum, &maximum, format, flags);
}

bool SliderInt(const char* label, int* value, const int minimum, const int maximum,
    const char* format, const ImGuiSliderFlags flags) noexcept {
    return SliderScalar(label, ImGuiDataType_S32, value, &minimum, &maximum, format, flags);
}

ImVec4 ControlAccent() noexcept {
    const auto& accent = ue5mem::PlatformUiTheme().accent;
    return ImVec4(accent.red, accent.green, accent.blue, 1.0f);
}

bool BeginCombo(const char* label, const char* preview, const ImGuiComboFlags flags) noexcept {
    ImGuiWindow* const window = ImGui::GetCurrentWindow();
    if (window == nullptr || window->SkipItems) return false;
    // The frame is measured before the call: once open, the combo makes its
    // popup the current window.
    const ImVec2 position = window->DC.CursorPos;
    const float width = (flags & ImGuiComboFlags_NoPreview) != 0
        ? ImGui::GetFrameHeight() : ImGui::CalcItemWidth();
    const float height = ImGui::GetFrameHeight();
    ImDrawList* const list = window->DrawList;
    const bool open = ImGui::BeginCombo(label, preview, flags | ImGuiComboFlags_NoArrowButton);
    const float scale = CurrentScale();
    const float half = (std::max)(3.0f, height * 0.13f);
    const ImVec2 center(position.x + width - height * 0.5f, position.y + height * 0.5f);
    ImVec4 ink = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ink.w *= 0.70f;
    const float lift = open ? -half * 0.25f : half * 0.25f;
    const float tip = open ? -half * 0.55f : half * 0.55f;
    list->PathLineTo(ImVec2(center.x - half, center.y - tip + lift));
    list->PathLineTo(ImVec2(center.x, center.y + tip + lift));
    list->PathLineTo(ImVec2(center.x + half, center.y - tip + lift));
    list->PathStroke(ImGui::GetColorU32(ink), 0, (std::max)(1.4f, 1.5f * scale));
    return open;
}

bool Combo(const char* label, int* current, const char* const items[], const int count) noexcept {
    if (current == nullptr || items == nullptr || count <= 0) return false;
    const char* const preview = *current >= 0 && *current < count ? items[*current] : "";
    bool changed = false;
    if (BeginCombo(label, preview)) {
        for (int index = 0; index < count; ++index) {
            ImGui::PushID(index);
            const bool selected = index == *current;
            if (ImGui::Selectable(items[index], selected)) {
                changed = changed || !selected;
                *current = index;
            }
            if (selected) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

}  // namespace anomaly::glass
