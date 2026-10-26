// Headless verification for the liquid glass material on a D3D12 device.
//
// The in-game overlay records the material into the game's own command list, so
// this probe reproduces exactly that: an off-screen render target stands in for
// the back buffer, the material is initialized for D3D12, a glass surface is
// drawn through the same command plumbing ImGui uses, and the result is read back
// to a BMP. It is a development fixture: it never runs inside a game.

#include "anomaly/liquid_glass.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <imgui.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr UINT kWidth = 900;
constexpr UINT kHeight = 600;
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

template <typename T>
class Com final {
public:
    Com() = default;
    ~Com() { Reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;

    T** Put() { return resource_.ReleaseAndGetAddressOf(); }
    T* operator->() const { return resource_.Get(); }
    T* Get() const { return resource_.Get(); }
    explicit operator bool() const { return resource_ != nullptr; }
    void Reset() { resource_.Reset(); }

private:
    Microsoft::WRL::ComPtr<T> resource_;
};

bool WriteBmp(const std::filesystem::path& path, const std::vector<std::uint8_t>& rgba,
    const UINT width, const UINT height) {
    const UINT row_bytes = width * 4U;
    const UINT image_bytes = row_bytes * height;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    BITMAPFILEHEADER header{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(width);
    info.biHeight = -static_cast<LONG>(height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = image_bytes;
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(info);
    header.bfSize = header.bfOffBits + image_bytes;
    stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
    stream.write(reinterpret_cast<const char*>(&info), sizeof(info));
    stream.write(reinterpret_cast<const char*>(rgba.data()), image_bytes);
    return stream.good();
}

void RunCallbacks(ImDrawData* const draw_data) {
    if (draw_data == nullptr) return;
    for (int index = 0; index < draw_data->CmdListsCount; ++index) {
        const ImDrawList* const list = draw_data->CmdLists[index];
        for (const ImDrawCmd& command : list->CmdBuffer) {
            if (command.UserCallback == nullptr) continue;
            if (command.UserCallback != ImDrawCallback_ResetRenderState) {
                command.UserCallback(list, &command);
            }
        }
    }
}

}  // namespace

// The debug layer states exactly what a rejected pipeline descriptor got wrong;
// without it CreateGraphicsPipelineState only reports E_INVALIDARG.
void DumpDebugMessages(ID3D12Device* const device, const char* const stage) {
    Com<ID3D12InfoQueue> queue;
    if (device == nullptr || FAILED(device->QueryInterface(IID_PPV_ARGS(queue.Put())))) return;
    const UINT64 count = queue->GetNumStoredMessages();
    for (UINT64 index = 0; index < count; ++index) {
        SIZE_T size{};
        if (FAILED(queue->GetMessage(index, nullptr, &size)) || size == 0) continue;
        std::vector<char> buffer(size);
        auto* const message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        if (FAILED(queue->GetMessage(index, message, &size))) continue;
        std::printf("[%s] %s\n", stage,
            message->pDescription != nullptr ? message->pDescription : "(no description)");
    }
    queue->ClearStoredMessages();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    Com<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.Put())))) {
        debug->EnableDebugLayer();
        std::printf("debug layer enabled\n");
    }
    Com<ID3D12Device> device;    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.Put())))) {
        std::printf("device creation failed\n");
        return 1;
    }
    const D3D12_RESOURCE_DESC resource_description = [&] {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = kWidth;
        description.Height = kHeight;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = kFormat;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        return description;
    }();
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    Com<ID3D12Resource> target;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &resource_description, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
            IID_PPV_ARGS(target.Put())))) {
        std::printf("target creation failed\n");
        return 1;
    }
    Com<ID3D12DescriptorHeap> rtv_heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_description.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(rtv_heap.Put())))) {
        std::printf("rtv heap failed\n");
        return 1;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);

    Com<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(queue.Put())))) {
        std::printf("queue failed\n");
        return 1;
    }
    Com<ID3D12CommandAllocator> allocator;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(allocator.Put())))) {
        std::printf("allocator failed\n");
        return 1;
    }
    Com<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
            nullptr, IID_PPV_ARGS(list.Put())))) {
        std::printf("command list failed\n");
        return 1;
    }

    // Stands in for the host's shader-visible heap: the material must hand it
    // back after its draws, exactly as it does in the overlay.
    Com<ID3D12DescriptorHeap> host_heap;
    {
        D3D12_DESCRIPTOR_HEAP_DESC host_description{};
        host_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        host_description.NumDescriptors = 2;
        host_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&host_description,
                IID_PPV_ARGS(host_heap.Put())))) {
            std::printf("host heap failed\n");
            return 1;
        }
    }
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight));
    io.DeltaTime = 1.0f / 60.0f;
    DumpDebugMessages(device.Get(), "before");
    if (!anomaly::glass::InitializeRendererD3D12(device.Get(), kFormat)) {
        std::printf("glass d3d12 initialisation failed\n");
        DumpDebugMessages(device.Get(), "init");
        return 1;
    }
    std::printf("glass renderer available: %d\n", anomaly::glass::RendererAvailable() ? 1 : 0);

    // The overlay's own frame: the backdrop is already on the target, then the UI
    // records capture + surface commands into it.
    // No ImGui backend runs here, so the commands are recorded into a draw list
    // of our own -- the same shape the overlay's window underlays have.
    ImDrawList draw_list(ImGui::GetDrawListSharedData());
    draw_list._ResetForNewFrame();
    draw_list.PushClipRect(ImVec2(0.0f, 0.0f),
        ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight)));
    draw_list.PushTextureID(io.Fonts->TexID);
    anomaly::glass::CaptureBackdrop(&draw_list);
    anomaly::glass::SurfaceOptions options;
    options.variant = anomaly::glass::Variant::Regular;
    options.rounding = 26.0f;
    options.scale = 1.0f;
    options.clip = true;
    anomaly::glass::DrawSurface(&draw_list, ImVec2(140.0f, 110.0f), ImVec2(760.0f, 470.0f),
        options);
    draw_list._PopUnusedDrawCmd();
    // Clear the target to a flat backdrop, then let the material run.
    const float backdrop[4]{0.16f, 0.34f, 0.62f, 1.0f};
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->ClearRenderTargetView(rtv, backdrop, 0, nullptr);
    // Hard-edged patches behind the pane: a blurred face has to smear them, and
    // a refracting rim has to bend them, which a flat backdrop cannot show.
    const float red[4]{0.85f, 0.20f, 0.16f, 1.0f};
    const float green[4]{0.18f, 0.72f, 0.34f, 1.0f};
    const float amber[4]{0.95f, 0.78f, 0.22f, 1.0f};
    const D3D12_RECT left_edge{0, 0, 260, static_cast<LONG>(kHeight)};
    const D3D12_RECT top_right{560, 0, static_cast<LONG>(kWidth), 210};
    const D3D12_RECT bottom_strip{260, 470, 560, static_cast<LONG>(kHeight)};
    list->ClearRenderTargetView(rtv, red, 1, &left_edge);
    list->ClearRenderTargetView(rtv, green, 1, &top_right);
    list->ClearRenderTargetView(rtv, amber, 1, &bottom_strip);
    anomaly::glass::BeginD3D12Frame(list.Get(), target.Get(), rtv, host_heap.Get());
    for (const ImDrawCmd& command : draw_list.CmdBuffer) {
        if (command.UserCallback != nullptr &&
            command.UserCallback != ImDrawCallback_ResetRenderState) {
            command.UserCallback(&draw_list, &command);
        }
    }

    const auto transition = [&](const D3D12_RESOURCE_STATES before,
                                const D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = target.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        list->ResourceBarrier(1, &barrier);
    };
    transition(D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows{};
    UINT64 row_size{};
    UINT64 total{};
    device->GetCopyableFootprints(&resource_description, 0, 1, 0, &footprint, &rows, &row_size,
        &total);
    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer_description{};
    buffer_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_description.Width = total;
    buffer_description.Height = 1;
    buffer_description.DepthOrArraySize = 1;
    buffer_description.MipLevels = 1;
    buffer_description.SampleDesc.Count = 1;
    buffer_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Com<ID3D12Resource> readback;
    if (FAILED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
            &buffer_description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(readback.Put())))) {
        std::printf("readback creation failed\n");
        return 1;
    }
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = target.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    if (FAILED(list->Close())) {
        std::printf("close failed\n");
        return 1;
    }
    ID3D12CommandList* lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
    Com<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.Put())))) {
        std::printf("fence failed\n");
        return 1;
    }
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, event);
        WaitForSingleObject(event, 10000);
    }
    CloseHandle(event);

    void* mapped{};
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(total)};
    if (FAILED(readback->Map(0, &read_range, &mapped))) {
        std::printf("map failed\n");
        return 1;
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth) * kHeight * 4U);
    const auto* const source_bytes = static_cast<const std::uint8_t*>(mapped);
    for (UINT row = 0; row < kHeight; ++row) {
        std::memcpy(pixels.data() + static_cast<std::size_t>(row) * kWidth * 4U,
            source_bytes + footprint.Offset +
            static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
            static_cast<std::size_t>(kWidth) * 4U);
    }
    const D3D12_RANGE written_range{0, 0};
    readback->Unmap(0, &written_range);

    const auto path = std::filesystem::current_path() / "glass-d3d12.bmp";
    if (!WriteBmp(path, pixels, kWidth, kHeight)) {
        std::printf("write failed\n");
        return 1;
    }
    // A center pixel is inside the surface, a corner pixel is outside it: the two
    // must differ, which is what proves the pass actually drew.
    const auto pixel = [&pixels](const UINT x, const UINT y) {
        const std::size_t index = (static_cast<std::size_t>(y) * kWidth + x) * 4U;
        return std::uint32_t(pixels[index]) | (std::uint32_t(pixels[index + 1]) << 8U) |
            (std::uint32_t(pixels[index + 2]) << 16U);
    };
    std::printf("corner=%06X center=%06X\n", pixel(8, 8), pixel(kWidth / 2, kHeight / 2));
    std::printf("wrote %s\n", path.string().c_str());
    anomaly::glass::ShutdownRenderer();
    ImGui::DestroyContext();
    return 0;
}
