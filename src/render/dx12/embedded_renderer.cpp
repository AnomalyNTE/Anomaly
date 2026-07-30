#include "embedded_host_internal.hpp"
#include "anomaly/host_ui_service.hpp"
#include "anomaly/platform_ui_input_policy.hpp"
#include "anomaly/platform_ui_theme.hpp"
#include "anomaly/structured_logger.hpp"

#include "embedded_ui_resource_render_backend.hpp"

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <cstring>
#include <functional>
#include <limits>
#include <sstream>
#include <utility>

namespace ue5mem::embedded {
namespace {

void AllocateSrv(
    ImGui_ImplDX12_InitInfo* info,
    D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
    D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    if (cpu != nullptr) *cpu = {};
    if (gpu != nullptr) *gpu = {};
    if (info == nullptr || cpu == nullptr || gpu == nullptr) return;
    auto* state = static_cast<EmbeddedState*>(info->UserData);
    if (state == nullptr) return;
    static_cast<void>(state->shader_descriptors.AllocateReserved(cpu, gpu));
}

void FreeSrv(
    ImGui_ImplDX12_InitInfo* info,
    D3D12_CPU_DESCRIPTOR_HANDLE cpu,
    D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
    if (info == nullptr) return;
    auto* state = static_cast<EmbeddedState*>(info->UserData);
    if (state == nullptr) return;
    static_cast<void>(state->shader_descriptors.Free(cpu, gpu));
}

float HostDpiScale(const HWND window) noexcept {
    if (window == nullptr || !IsWindow(window)) return 0.0F;
    const UINT dpi = GetDpiForWindow(window);
    return dpi == 0 ? 0.0F : static_cast<float>(dpi) / 96.0F;
}

void SynchronizeHostFontScale(EmbeddedState& state) noexcept {
    if (state.plugins == nullptr) return;
    const float scale = HostDpiScale(state.window);
    if (scale > 0.0F) {
        static_cast<void>(state.plugins->UiResources().SetHostFontScale(scale));
    }
}

bool WaitForFrame(
    EmbeddedState& state,
    FrameContext& frame,
    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    if (frame.fence_value == 0 || state.fence == nullptr ||
        state.fence->GetCompletedValue() >= frame.fence_value) return true;
    if (state.fence_event == nullptr ||
        FAILED(state.fence->SetEventOnCompletion(frame.fence_value, state.fence_event))) {
        return false;
    }
    const auto bounded = (std::max)(timeout, std::chrono::milliseconds::zero());
    const auto maximum = static_cast<std::int64_t>(INFINITE - 1);
    const DWORD wait_milliseconds = static_cast<DWORD>(
        (std::min)(bounded.count(), maximum));
    if (WaitForSingleObject(state.fence_event, wait_milliseconds) != WAIT_OBJECT_0) {
        return false;
    }
    return state.fence->GetCompletedValue() >= frame.fence_value;
}

constexpr UINT kPixelProbeWidth = 32;
constexpr UINT kPixelProbeHeight = 32;
constexpr UINT64 kMaximumPixelProbeBytes = 1024ULL * 1024ULL;
constexpr std::uint32_t kPixelProbeRetryFrames = 30;

std::uint64_t CaptureGeneration(const EmbeddedState& state) noexcept {
    if (!state.diagnostics.capture_generation) return 0;
    try {
        return state.diagnostics.capture_generation();
    } catch (...) {
        return 0;
    }
}

void NotifyRenderThread(
    const EmbeddedState& state, std::uint64_t capture_generation) noexcept {
    if (!state.diagnostics.render) return;
    try {
        state.diagnostics.render(capture_generation, GetCurrentThreadId());
    } catch (...) {
    }
}

void NotifyPixelProbe(
    const EmbeddedState& state,
    std::uint64_t capture_generation,
    bool non_empty,
    bool success) noexcept {
    if (!state.diagnostics.pixel_probe) return;
    try {
        state.diagnostics.pixel_probe(capture_generation, non_empty, success);
    } catch (...) {
    }
}

void ReleasePixelProbeResources(PixelProbeState& probe) noexcept {
    Release(probe.before_overlay);
    Release(probe.after_overlay);
    probe.footprint = {};
    probe.row_count = 0;
    probe.row_size_bytes = 0;
    probe.total_bytes = 0;
    probe.fence_value = 0;
    probe.ticket = {};
    probe.command_recorded = false;
    probe.pending = false;
}

enum class PixelProbeReadback : std::uint8_t {
    Failed,
    Identical,
    Different,
};

PixelProbeReadback ReadbackResult(PixelProbeState& probe) noexcept {
    if (probe.before_overlay == nullptr || probe.after_overlay == nullptr ||
        !SmokeProbeFootprintValid(
            probe.row_count, probe.row_size_bytes,
            probe.footprint.Footprint.RowPitch, probe.total_bytes,
            kMaximumPixelProbeBytes)) {
        return PixelProbeReadback::Failed;
    }
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(probe.total_bytes)};
    void* before_mapping{};
    void* after_mapping{};
    if (FAILED(probe.before_overlay->Map(0, &read_range, &before_mapping)) ||
        before_mapping == nullptr) {
        return PixelProbeReadback::Failed;
    }
    if (FAILED(probe.after_overlay->Map(0, &read_range, &after_mapping)) ||
        after_mapping == nullptr) {
        const D3D12_RANGE written_range{};
        probe.before_overlay->Unmap(0, &written_range);
        return PixelProbeReadback::Failed;
    }

    const auto* before = static_cast<const std::uint8_t*>(before_mapping) +
        probe.footprint.Offset;
    const auto* after = static_cast<const std::uint8_t*>(after_mapping) +
        probe.footprint.Offset;
    bool differs{};
    for (UINT row = 0; row < probe.row_count && !differs; ++row) {
        const std::size_t offset =
            static_cast<std::size_t>(row) * probe.footprint.Footprint.RowPitch;
        differs = std::memcmp(
            before + offset, after + offset,
            static_cast<std::size_t>(probe.row_size_bytes)) != 0;
    }

    const D3D12_RANGE written_range{};
    probe.after_overlay->Unmap(0, &written_range);
    probe.before_overlay->Unmap(0, &written_range);
    return differs ? PixelProbeReadback::Different : PixelProbeReadback::Identical;
}

bool CompletePixelProbe(
    EmbeddedState& state, std::uint64_t observed_generation) noexcept {
    auto& probe = state.pixel_probe;
    if (!probe.pending || state.fence == nullptr || probe.fence_value == 0) {
        return false;
    }
    // Query the device before touching the fence or mapped readback memory.
    // A removed device may expose UINT64_MAX as its completed fence value;
    // both signals are terminal evidence failures rather than completed work.
    const bool device_ready = state.device != nullptr &&
        SUCCEEDED(state.device->GetDeviceRemovedReason());
    const UINT64 completed = device_ready
        ? state.fence->GetCompletedValue()
        : (std::numeric_limits<UINT64>::max)();
    const SmokeProbeFenceState fence_state = ClassifySmokeProbeFence(
        device_ready, completed, probe.fence_value);
    if (fence_state == SmokeProbeFenceState::Pending) {
        return false;
    }
    const SmokeProbeTicket ticket = probe.ticket;
    const PixelProbeReadback result = fence_state == SmokeProbeFenceState::Ready
        ? ReadbackResult(probe)
        : PixelProbeReadback::Failed;
    ReleasePixelProbeResources(probe);
    const bool readback_succeeded = result != PixelProbeReadback::Failed;
    const bool differs = result == PixelProbeReadback::Different;
    CompleteSmokeProbePolicy(
        probe.policy, ticket, observed_generation,
        readback_succeeded, differs, kPixelProbeRetryFrames);
    // Preserve the generation captured when the GPU commands were recorded.
    // The evidence session owns the final expected-generation check, so even
    // a late completion can be rejected atomically without being relabelled.
    if (ticket.capture_generation != 0) {
        NotifyPixelProbe(
            state, ticket.capture_generation, differs, readback_succeeded);
    }
    return true;
}

bool CreateReadbackBuffer(
    ID3D12Device* device, UINT64 size, ID3D12Resource** destination) noexcept {
    if (device == nullptr || destination == nullptr || size == 0 ||
        size > kMaximumPixelProbeBytes) {
        return false;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = size;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return SUCCEEDED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(destination)));
}

bool PreparePixelProbe(
    EmbeddedState& state,
    const FrameContext& frame,
    std::uint64_t capture_generation) noexcept {
    auto& probe = state.pixel_probe;
    if (probe.pending || probe.command_recorded ||
        state.device == nullptr || frame.back_buffer == nullptr) {
        return false;
    }
    if (!SmokeProbeReady(probe.policy)) return false;

    const D3D12_RESOURCE_DESC back_buffer = frame.back_buffer->GetDesc();
    if (capture_generation == 0 ||
        back_buffer.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        back_buffer.SampleDesc.Count != 1 ||
        back_buffer.Format == DXGI_FORMAT_UNKNOWN ||
        back_buffer.Width == 0 || back_buffer.Height == 0) {
        CompleteSmokeProbePolicy(
            probe.policy, CurrentSmokeProbeTicket(probe.policy),
            capture_generation, false, false, kPixelProbeRetryFrames);
        return false;
    }
    const SmokeProbeRegion region = ClampSmokeProbeRegion(
        back_buffer.Width, back_buffer.Height,
        kPixelProbeWidth, kPixelProbeHeight);
    if (!region) return false;
    D3D12_RESOURCE_DESC texture = back_buffer;
    texture.Width = region.width;
    texture.Height = region.height;
    texture.Flags = D3D12_RESOURCE_FLAG_NONE;
    state.device->GetCopyableFootprints(
        &texture, 0, 1, 0, &probe.footprint, &probe.row_count,
        &probe.row_size_bytes, &probe.total_bytes);
    if (!SmokeProbeFootprintValid(
            probe.row_count, probe.row_size_bytes,
            probe.footprint.Footprint.RowPitch, probe.total_bytes,
            kMaximumPixelProbeBytes) ||
        !CreateReadbackBuffer(
            state.device, probe.total_bytes, &probe.before_overlay) ||
        !CreateReadbackBuffer(
            state.device, probe.total_bytes, &probe.after_overlay)) {
        ReleasePixelProbeResources(probe);
        CompleteSmokeProbePolicy(
            probe.policy, CurrentSmokeProbeTicket(probe.policy),
            capture_generation, false, false, kPixelProbeRetryFrames);
        return false;
    }
    probe.ticket = CurrentSmokeProbeTicket(probe.policy);
    probe.command_recorded = true;
    return true;
}

D3D12_RESOURCE_BARRIER Transition(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

void CopyBackBuffer(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* back_buffer,
    ID3D12Resource* readback,
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint) noexcept {
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = back_buffer;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX source_box{
        0, 0, 0,
        footprint.Footprint.Width,
        footprint.Footprint.Height,
        1};
    command_list->CopyTextureRegion(
        &destination, 0, 0, 0, &source, &source_box);
}

bool ReleaseBackBuffersForResize(EmbeddedState& state) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (auto& frame : state.frames) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline || !WaitForFrame(
                state, frame,
                std::chrono::ceil<std::chrono::milliseconds>(deadline - now))) {
            const std::uint64_t capture_generation = CaptureGeneration(state);
            SynchronizeSmokeProbeCapture(
                state.pixel_probe.policy, capture_generation);
            static_cast<void>(CompletePixelProbe(state, capture_generation));
            return false;
        }
    }
    const std::uint64_t capture_generation = CaptureGeneration(state);
    SynchronizeSmokeProbeCapture(state.pixel_probe.policy, capture_generation);
    static_cast<void>(CompletePixelProbe(state, capture_generation));
    ReleasePixelProbeResources(state.pixel_probe);
    for (auto& frame : state.frames) {
        Release(frame.back_buffer);
        frame.fence_value = 0;
    }
    return true;
}

bool SameComObject(IUnknown* left, IUnknown* right) noexcept {
    if (left == nullptr || right == nullptr) return left == right;
    IUnknown* left_identity{};
    IUnknown* right_identity{};
    const bool compared = SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&left_identity))) &&
        SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&right_identity)));
    const bool same = compared && left_identity == right_identity;
    Release(left_identity);
    Release(right_identity);
    return same;
}

bool QueueUsesDevice(ID3D12CommandQueue* queue, ID3D12Device* device) noexcept {
    if (queue == nullptr || device == nullptr ||
        queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return false;
    }
    ID3D12Device* queue_device{};
    const bool queried = SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&queue_device)));
    const bool same = queried && SameComObject(queue_device, device);
    Release(queue_device);
    return same;
}

void ClearPendingResizeQueue(EmbeddedState& state) noexcept {
    Release(state.pending_resize_queue);
    state.pending_resize_queue_valid = true;
}

void SelectPendingResizeQueue(
    EmbeddedState& state,
    UINT present_queue_count,
    IUnknown* const* present_queues,
    bool validate_present_queues) noexcept {
    ClearPendingResizeQueue(state);
    if (!validate_present_queues) return;
    const UINT queue_count = present_queue_count != 0
        ? present_queue_count
        : static_cast<UINT>(state.frames.size());
    if (queue_count == 0 || present_queues == nullptr) {
        state.pending_resize_queue_valid = false;
        return;
    }

    ID3D12CommandQueue* selected{};
    for (UINT index = 0; index < queue_count; ++index) {
        ID3D12CommandQueue* candidate{};
        if (present_queues[index] == nullptr ||
            FAILED(present_queues[index]->QueryInterface(IID_PPV_ARGS(&candidate))) ||
            !QueueUsesDevice(candidate, state.device) ||
            (selected != nullptr && !SameComObject(selected, candidate))) {
            Release(candidate);
            Release(selected);
            state.pending_resize_queue_valid = false;
            return;
        }
        if (selected == nullptr) {
            selected = candidate;
        } else {
            Release(candidate);
        }
    }
    state.pending_resize_queue = selected;
}

enum class ResizeQueueAdoption : std::uint8_t {
    Invalid,
    Preserved,
    Changed,
};

bool InvalidatePluginUiDevice(EmbeddedState& state) noexcept {
    if (!state.plugin_ui_device_active) return true;
    std::unique_lock plugin_lock(*state.plugin_mutex, std::try_to_lock);
    if (!plugin_lock.owns_lock()) return false;
    if (state.plugins != nullptr) state.plugins->OnUiDeviceLost();
    state.plugin_ui_device_active = false;
    return true;
}

bool RebuildPluginUiDevice(EmbeddedState& state) noexcept {
    if (!state.platform_ui_initialized) return true;
    std::unique_lock plugin_lock(*state.plugin_mutex, std::try_to_lock);
    if (!plugin_lock.owns_lock() || state.plugins == nullptr) return false;
    // Keep the flag set on an acknowledgement failure so ReleaseGraphics can
    // invalidate the generation before destroying the backend objects.
    state.plugin_ui_device_active = true;
    return state.plugins->OnUiDeviceRebuilt();
}

ResizeQueueAdoption AdoptPendingResizeQueue(EmbeddedState& state) noexcept {
    if (!state.pending_resize_queue_valid) {
        ClearPendingResizeQueue(state);
        return ResizeQueueAdoption::Invalid;
    }
    if (state.pending_resize_queue == nullptr ||
        SameComObject(state.pending_resize_queue, state.render_queue)) {
        ClearPendingResizeQueue(state);
        return ResizeQueueAdoption::Preserved;
    }
    if (state.dx12_initialized) {
        if (!InvalidatePluginUiDevice(state)) return ResizeQueueAdoption::Invalid;
        state.shader_descriptors.ReleaseReserved();
        ImGui_ImplDX12_Shutdown();
        state.dx12_initialized = false;
    }
    Release(state.render_queue);
    state.render_queue = std::exchange(state.pending_resize_queue, nullptr);
    state.pending_resize_queue_valid = true;
    return ResizeQueueAdoption::Changed;
}

bool ReconfigureFramesAfterResize(
    EmbeddedState& state,
    const DXGI_SWAP_CHAIN_DESC& description,
    bool force_renderer_reconfigure) {
    const bool frame_layout_changed = description.BufferCount != state.frames.size() ||
        description.BufferDesc.Format != state.render_target_format;
    if (!frame_layout_changed && !force_renderer_reconfigure) return true;
    if (state.dx12_initialized) {
        if (!InvalidatePluginUiDevice(state)) return false;
        state.shader_descriptors.ReleaseReserved();
        ImGui_ImplDX12_Shutdown();
        state.dx12_initialized = false;
    }
    if (frame_layout_changed) {
        for (auto& frame : state.frames) Release(frame.allocator);
        state.frames.clear();
        Release(state.render_target_heap);

        D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
        heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_description.NumDescriptors = description.BufferCount;
        if (description.BufferCount == 0 || FAILED(state.device->CreateDescriptorHeap(
                &heap_description, IID_PPV_ARGS(&state.render_target_heap)))) return false;

        state.frames.resize(description.BufferCount);
        for (auto& frame : state.frames) {
            if (FAILED(state.device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator)))) return false;
        }
    }

    ImGui_ImplDX12_InitInfo init_info{};
    init_info.Device = state.device;
    init_info.CommandQueue = state.render_queue;
    init_info.NumFramesInFlight = static_cast<int>(state.frames.size());
    init_info.RTVFormat = description.BufferDesc.Format;
    init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    init_info.SrvDescriptorHeap = state.shader_heap;
    init_info.UserData = &state;
    init_info.SrvDescriptorAllocFn = AllocateSrv;
    init_info.SrvDescriptorFreeFn = FreeSrv;
    if (!ImGui_ImplDX12_Init(&init_info)) return false;
    state.dx12_initialized = true;
    state.render_target_format = description.BufferDesc.Format;
    return RebuildPluginUiDevice(state);
}

bool RebuildBackBuffersAfterResize(
    EmbeddedState& state, bool force_renderer_reconfigure = false) {
    if (state.source_swap_chain == nullptr || state.device == nullptr ||
        state.render_target_heap == nullptr) return false;
    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(state.source_swap_chain->GetDesc(&description)) ||
        !ReconfigureFramesAfterResize(
            state, description, force_renderer_reconfigure)) return false;

    auto render_target = state.render_target_heap->GetCPUDescriptorHandleForHeapStart();
    const UINT increment = state.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT index = 0; index < description.BufferCount; ++index) {
        auto& frame = state.frames[index];
        if (FAILED(state.source_swap_chain->GetBuffer(
                index, IID_PPV_ARGS(&frame.back_buffer)))) {
            for (auto& acquired : state.frames) Release(acquired.back_buffer);
            return false;
        }
        frame.render_target = render_target;
        state.device->CreateRenderTargetView(
            frame.back_buffer, nullptr, frame.render_target);
        render_target.ptr += increment;
    }
    RECT output_rect{};
    if (state.window != nullptr && GetClientRect(state.window, &output_rect)) {
        const LONG width = (std::max)(0L, output_rect.right - output_rect.left);
        const LONG height = (std::max)(0L, output_rect.bottom - output_rect.top);
        state.selected_area =
            static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    }
    return true;
}

void UpdateMenuCursor(EmbeddedState& state, bool active) {
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = active;
    if (active) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    } else {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
    }
    if (active) {
        if (!state.menu_cursor_active) {
            io.ClearEventsQueue();
            io.ClearInputMouse();
            state.previous_capture = GetCapture();
            state.cursor_clip_saved = GetClipCursor(&state.previous_cursor_clip) != FALSE;
            ReleaseCapture();
            state.menu_cursor_active = true;
        }
        ClipCursor(nullptr);
        return;
    }
    if (!state.menu_cursor_active) return;
    io.ClearEventsQueue();
    io.ClearInputKeys();
    io.ClearInputMouse();
    if (state.cursor_clip_saved) ClipCursor(&state.previous_cursor_clip);
    if (state.previous_capture != nullptr && IsWindow(state.previous_capture)) {
        SetCapture(state.previous_capture);
    }
    state.previous_capture = nullptr;
    state.cursor_clip_saved = false;
    state.menu_cursor_active = false;
    // The game owns the native cursor once menu capture ends. Synthesizing a
    // cursor message here races its WndProc and causes visible flicker.
}

std::uint64_t SwapChainArea(IDXGISwapChain* swap_chain) {
    DXGI_SWAP_CHAIN_DESC description{};
    if (swap_chain == nullptr || FAILED(swap_chain->GetDesc(&description))) return 0;
    RECT rectangle{};
    if (description.OutputWindow == nullptr ||
        !GetClientRect(description.OutputWindow, &rectangle)) return 0;
    const auto width = (std::max)(0L, rectangle.right - rectangle.left);
    const auto height = (std::max)(0L, rectangle.bottom - rectangle.top);
    return static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
}

bool PreferSwapChain(const EmbeddedState& state, IDXGISwapChain* candidate) {
    if (state.source_swap_chain == candidate) return false;
    if (state.window == nullptr || !IsWindow(state.window) || !IsWindowVisible(state.window)) return true;
    const std::uint64_t candidate_area = SwapChainArea(candidate);
    return candidate_area > state.selected_area + state.selected_area / 4;
}

bool InitializeGraphics(EmbeddedState& state, IDXGISwapChain* swap_chain) {
    if (state.plugins == nullptr || state.quarantined_plugin_owner != nullptr ||
        PlatformUiQuarantined(state.plugins)) {
        // A retained owner is a generation fence. Do not allocate another
        // ImGui/device generation until the prior UI callbacks have drained.
        return false;
    }
    // A previous device generation may have stopped accepting UI work but
    // still be waiting for an untracked callback. Finish that owner handoff
    // before allocating a new device/context generation.
    if (state.platform_ui_initialized &&
        !ReleaseGraphics(state, RendererLifecycle::Cold)) {
        return false;
    }
    state.renderer = RendererLifecycle::Discovering;
    ID3D12CommandQueue* queue{};
    {
        std::scoped_lock lock(state.queue_mutex);
        queue = state.captured_queue;
        if (queue != nullptr) queue->AddRef();
    }
    if (queue == nullptr) {
        state.renderer = RendererLifecycle::Cold;
        return false;
    }

    IDXGISwapChain3* swap_chain3{};
    ID3D12Device* device{};
    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(swap_chain->QueryInterface(IID_PPV_ARGS(&swap_chain3))) ||
        FAILED(swap_chain->GetDevice(IID_PPV_ARGS(&device))) ||
        FAILED(swap_chain->GetDesc(&description)) || description.BufferCount == 0 ||
        description.OutputWindow == nullptr) {
        Release(queue);
        Release(device);
        Release(swap_chain3);
        state.renderer = RendererLifecycle::Cold;
        return false;
    }
    DWORD output_process{};
    GetWindowThreadProcessId(description.OutputWindow, &output_process);
    RECT output_rect{};
    GetClientRect(description.OutputWindow, &output_rect);
    const LONG width = output_rect.right - output_rect.left;
    const LONG height = output_rect.bottom - output_rect.top;
    if (output_process != GetCurrentProcessId() || !IsWindowVisible(description.OutputWindow) ||
        width < 640 || height < 360) {
        Release(queue);
        Release(device);
        Release(swap_chain3);
        state.renderer = RendererLifecycle::Cold;
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC render_target_heap_description{};
    render_target_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    render_target_heap_description.NumDescriptors = description.BufferCount;
    D3D12_DESCRIPTOR_HEAP_DESC shader_heap_description{};
    shader_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    shader_heap_description.NumDescriptors = kEmbeddedShaderResourceDescriptorCapacity;
    shader_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(
            &render_target_heap_description, IID_PPV_ARGS(&state.render_target_heap))) ||
        FAILED(device->CreateDescriptorHeap(
            &shader_heap_description, IID_PPV_ARGS(&state.shader_heap))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state.fence)))) {
        Release(queue);
        Release(device);
        Release(swap_chain3);
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    const UINT shader_descriptor_increment = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!state.shader_descriptors.Configure(
            state.shader_heap->GetCPUDescriptorHandleForHeapStart(),
            state.shader_heap->GetGPUDescriptorHandleForHeapStart(),
            shader_descriptor_increment,
            kEmbeddedShaderResourceDescriptorCapacity)) {
        Release(queue);
        Release(device);
        Release(swap_chain3);
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    state.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (state.fence_event == nullptr) {
        Release(queue);
        Release(device);
        Release(swap_chain3);
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }

    state.frames.resize(description.BufferCount);
    auto render_target = state.render_target_heap->GetCPUDescriptorHandleForHeapStart();
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT index = 0; index < description.BufferCount; ++index) {
        auto& frame = state.frames[index];
        if (FAILED(device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator))) ||
            FAILED(swap_chain->GetBuffer(index, IID_PPV_ARGS(&frame.back_buffer)))) {
            Release(queue);
            Release(device);
            Release(swap_chain3);
            static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
            return false;
        }
        frame.render_target = render_target;
        device->CreateRenderTargetView(frame.back_buffer, nullptr, frame.render_target);
        render_target.ptr += increment;
    }
    if (FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, state.frames[0].allocator, nullptr,
            IID_PPV_ARGS(&state.command_list)))) {
        Release(queue);
        Release(device);
        Release(swap_chain3);
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    state.command_list->Close();

    state.swap_chain = swap_chain3;
    swap_chain->AddRef();
    state.source_swap_chain = swap_chain;
    state.device = device;
    state.window = description.OutputWindow;
    state.selected_area = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    IMGUI_CHECKVERSION();
    state.imgui_context = ImGui::CreateContext();
    if (state.imgui_context == nullptr) {
        Release(queue);
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    static_cast<void>(ConfigurePlatformUiFontAtlas(state.root));
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
    state.imgui_ini_path = (state.root / L"anomaly-imgui.ini").string();
    io.IniFilename = state.imgui_ini_path.c_str();
    if (!ImGui_ImplWin32_Init(state.window)) {
        Release(queue);
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    state.win32_initialized = true;
    ImGui_ImplDX12_InitInfo init_info{};
    init_info.Device = state.device;
    init_info.CommandQueue = queue;
    init_info.NumFramesInFlight = static_cast<int>(state.frames.size());
    init_info.RTVFormat = description.BufferDesc.Format;
    init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    init_info.SrvDescriptorHeap = state.shader_heap;
    init_info.UserData = &state;
    init_info.SrvDescriptorAllocFn = AllocateSrv;
    init_info.SrvDescriptorFreeFn = FreeSrv;
    const bool renderer_initialized = ImGui_ImplDX12_Init(&init_info);
    if (!renderer_initialized) {
        Release(queue);
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    state.render_queue = queue;
    state.render_target_format = description.BufferDesc.Format;
    state.dx12_initialized = true;
    if (!InstallEmbeddedInput(state)) {
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    {
        std::scoped_lock plugin_lock(*state.plugin_mutex);
        if (!state.lifecycle_invoker_bound) {
            const auto lifecycle_invoke = state.diagnostics.lifecycle_invoke;
            const auto lifecycle_post = state.diagnostics.lifecycle_post;
            const auto plugin_mutex = state.plugin_mutex;
            state.diagnostics.lifecycle_invoke = [lifecycle_invoke, plugin_mutex](
                std::function<void()> operation) -> std::uint32_t {
                auto guarded = [plugin_mutex, operation = std::move(operation)]() mutable {
                    std::scoped_lock lock(*plugin_mutex);
                    operation();
                };
                if (lifecycle_invoke) return lifecycle_invoke(std::move(guarded));
                try {
                    guarded();
                    return ERROR_SUCCESS;
                } catch (...) {
                    return ERROR_UNHANDLED_EXCEPTION;
                }
            };
            state.diagnostics.lifecycle_post = [lifecycle_post, lifecycle_invoke, plugin_mutex](
                std::function<void()> operation) -> std::uint32_t {
                auto guarded = [plugin_mutex, operation = std::move(operation)]() mutable {
                    std::scoped_lock lock(*plugin_mutex);
                    operation();
                };
                if (lifecycle_post) return lifecycle_post(std::move(guarded));
                if (lifecycle_invoke) return lifecycle_invoke(std::move(guarded));
                try {
                    guarded();
                    return ERROR_SUCCESS;
                } catch (...) {
                    return ERROR_UNHANDLED_EXCEPTION;
                }
            };
            state.lifecycle_invoker_bound = true;
        }
    }
    const auto plugin_owner = state.plugin_owner.lock();
    if (plugin_owner == nullptr ||
        !InitializePlatformUi(*state.plugins, state.diagnostics, plugin_owner)) {
        // A quarantined owner still references the previous generation. Do
        // not mark this context as initialized or publish a second UI owner.
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    {
        std::scoped_lock plugin_lock(*state.plugin_mutex);
        state.plugins->SetImGuiContext(ImGui::GetCurrentContext());
    }
    state.platform_ui_initialized = true;
    if (!state.ui_services.Publish(EmbeddedUiServiceTable())) {
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    const auto resource_backend = CreateEmbeddedUiResourceRenderBackend(state);
    if (resource_backend == nullptr) {
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    bool ui_device_rebuilt{};
    {
        std::scoped_lock plugin_lock(*state.plugin_mutex);
        SynchronizeHostFontScale(state);
        state.plugins->SetUiResourceRenderBackend(resource_backend);
        state.plugins->SetNteEscMenuHostAction(anomaly::RequestHostUiManagementExpansion);
        state.plugins->SetUiService(EmbeddedUiServiceTable());
        // The PluginManager owns logical font/texture generations. Do not
        // expose a fresh renderer generation until it has acknowledged the
        // current device generation.
        state.plugin_ui_device_active = true;
        ui_device_rebuilt = state.plugins->OnUiDeviceRebuilt();
    }
    if (!ui_device_rebuilt) {
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    state.renderer = RendererLifecycle::Ready;
    std::ostringstream message;
    message << "embedded=1 hwnd=" << state.window
            << " buffers=" << state.frames.size() << " ui_generation="
            << state.ui_services.Query().generation;
    if (state.diagnostics.logger != nullptr) {
        anomaly::LogDetails details;
        details.thread_domain = anomaly::LogThreadDomain::Render;
        details.event_id = "render.embedded_ready";
        static_cast<void>(state.diagnostics.logger->Log(
            anomaly::LogLevel::Info, "render", message.str(), std::move(details)));
    }
    return true;
}

}  // namespace

bool ReleaseGraphics(
    EmbeddedState& state, RendererLifecycle final_state, bool force_release) {
    state.renderer = final_state;
    if (state.imgui_context != nullptr &&
        ImGui::GetCurrentContext() != state.imgui_context) {
        // The renderer must never shut down a context owned by another
        // generation or host. Leave this generation retained for its render
        // owner to finish the handoff.
        return false;
    }
    std::uint64_t capture_generation = CaptureGeneration(state);
    SynchronizeSmokeProbeCapture(state.pixel_probe.policy, capture_generation);
    static_cast<void>(CompletePixelProbe(state, capture_generation));
    if (state.submission_unfenced) {
        const HRESULT removed_reason = state.device != nullptr
            ? state.device->GetDeviceRemovedReason()
            : DXGI_ERROR_DEVICE_REMOVED;
        if (SUCCEEDED(removed_reason)) {
            UINT64 submitted_value{};
            for (const auto& frame : state.frames) {
                submitted_value = (std::max)(submitted_value, frame.fence_value);
            }
            if (submitted_value == 0 || state.render_queue == nullptr ||
                state.fence == nullptr ||
                FAILED(state.render_queue->Signal(state.fence, submitted_value))) {
                return false;
            }
        } else {
            capture_generation = CaptureGeneration(state);
            SynchronizeSmokeProbeCapture(
                state.pixel_probe.policy, capture_generation);
            static_cast<void>(CompletePixelProbe(state, capture_generation));
            for (auto& frame : state.frames) frame.fence_value = 0;
            ReleasePixelProbeResources(state.pixel_probe);
        }
        state.submission_unfenced = false;
    }
    if (state.imgui_context != nullptr) UpdateMenuCursor(state, false);
    if (state.fence != nullptr) {
        for (auto& frame : state.frames) {
            if (!WaitForFrame(state, frame, std::chrono::milliseconds(100))) return false;
        }
    }
    capture_generation = CaptureGeneration(state);
    SynchronizeSmokeProbeCapture(state.pixel_probe.policy, capture_generation);
    static_cast<void>(CompletePixelProbe(state, capture_generation));
    ReleasePixelProbeResources(state.pixel_probe);
    state.ui_services.Withdraw(EmbeddedUiServiceTable());
    if (state.platform_ui_initialized) {
        // Teardown may need to drain a lifecycle callback which itself takes
        // the plugin mutex. Do this before taking that mutex here.
        if (force_release) {
            // The bounded stop path must establish quarantine before the
            // ImGui context is destroyed. If another owner transition is in
            // progress, leave this generation intact for its completion.
            if (!QuarantinePlatformUi(std::chrono::milliseconds(100))) return false;
        } else if (!ShutdownPlatformUi()) {
            return false;
        }
        state.platform_ui_initialized = false;
    }
    if (state.plugins != nullptr && PlatformUiQuarantined(state.plugins)) {
        return false;
    }
    {
        std::unique_lock plugin_lock(*state.plugin_mutex, std::try_to_lock);
        if (!plugin_lock.owns_lock()) return false;
        if (state.plugin_ui_device_active) {
            if (state.plugins != nullptr) state.plugins->OnUiDeviceLost();
            state.plugin_ui_device_active = false;
        }
        if (state.plugins != nullptr) {
            state.plugins->SetUiResourceRenderBackend({});
            state.plugins->SetNteEscMenuHostAction({});
            state.plugins->SetUiService(nullptr);
            state.plugins->SetImGuiContext(nullptr);
        }
    }
    RestoreEmbeddedInput(state);
    if (state.dx12_initialized) {
        state.shader_descriptors.ReleaseReserved();
        ImGui_ImplDX12_Shutdown();
        state.dx12_initialized = false;
    }
    state.shader_descriptors.Reset();
    if (state.win32_initialized) {
        ImGui_ImplWin32_Shutdown();
        state.win32_initialized = false;
    }
    if (state.imgui_context != nullptr) {
        ImGui::DestroyContext(state.imgui_context);
        state.imgui_context = nullptr;
    }
    if (state.fence_event != nullptr) {
        CloseHandle(state.fence_event);
        state.fence_event = nullptr;
    }
    for (auto& frame : state.frames) {
        Release(frame.back_buffer);
        Release(frame.allocator);
    }
    state.frames.clear();
    Release(state.fence);
    Release(state.command_list);
    Release(state.shader_heap);
    Release(state.render_target_heap);
    Release(state.device);
    ClearPendingResizeQueue(state);
    Release(state.render_queue);
    Release(state.swap_chain);
    Release(state.source_swap_chain);
    state.window = nullptr;
    state.render_target_format = DXGI_FORMAT_UNKNOWN;
    state.selected_area = 0;
    return true;
}

void RenderEmbedded(IDXGISwapChain* swap_chain, UINT flags) {
    auto* state = g_state.load(std::memory_order_acquire);
    if (state == nullptr || (flags & DXGI_PRESENT_TEST) != 0) return;
    {
    std::scoped_lock render_lock(state->render_mutex);
    const RendererLifecycle lifecycle = state->renderer.load();
    if (lifecycle == RendererLifecycle::ResizePending ||
        lifecycle == RendererLifecycle::Stopping || lifecycle == RendererLifecycle::Stopped) return;
    if (lifecycle == RendererLifecycle::Ready && state->source_swap_chain != swap_chain) {
        if (!PreferSwapChain(*state, swap_chain)) return;
        // A failed release means the previous UI/device generation is still
        // owned by an in-flight callback. Do not initialize a second
        // generation against the same PluginManager or ImGui context.
        if (!ReleaseGraphics(*state, RendererLifecycle::Cold)) return;
    }
    if (state->renderer.load() != RendererLifecycle::Ready &&
        !InitializeGraphics(*state, swap_chain)) return;
    if (state->source_swap_chain != swap_chain) return;

    unsigned toggle_key = state->config.platform_toggle_key;
    if (state->diagnostics.settings_snapshot) {
        const auto settings = state->diagnostics.settings_snapshot();
        if (settings.ready) toggle_key = settings.values.input_menu_toggle;
    }
    const int toggle_state = GetAsyncKeyState(static_cast<int>(toggle_key));
    if (anomaly::ShouldTogglePlatformMenus(
            PlatformUiCapturingHotkey(), toggle_state)) {
        anomaly::SetHostUiMenusCollapsed(!anomaly::HostUiMenusCollapsed());
    }
    static_cast<void>(ApplyHostUiManagementExpansionRequest());

    const UINT index = state->swap_chain->GetCurrentBackBufferIndex();
    if (index >= state->frames.size()) return;
    auto& frame = state->frames[index];
    const std::uint64_t capture_generation = CaptureGeneration(*state);
    SynchronizeSmokeProbeCapture(state->pixel_probe.policy, capture_generation);
    static_cast<void>(CompletePixelProbe(*state, capture_generation));
    if (!WaitForFrame(*state, frame)) return;
    if (FAILED(frame.allocator->Reset()) ||
        FAILED(state->command_list->Reset(frame.allocator, nullptr))) {
        static_cast<void>(ReleaseGraphics(*state, RendererLifecycle::DeviceLost));
        return;
    }
    const bool probing = capture_generation != 0 &&
        PreparePixelProbe(*state, frame, capture_generation);

    ImGui_ImplDX12_NewFrame();
    // NewFrame creates the ImGui font atlas first, reserving descriptor slot
    // zero before scoped texture uploads allocate from the shared heap.
    {
        std::scoped_lock plugin_lock(*state->plugin_mutex);
        SynchronizeHostFontScale(*state);
        state->plugins->PrepareUiResources();
        PreparePlatformUiResources();
    }
    UpdateMenuCursor(*state, anomaly::HostUiMenusCaptureMouse());
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    anomaly::PrepareHostUiFrame();
    bool input_frame_published{};
    {
        std::scoped_lock plugin_lock(*state->plugin_mutex);
        input_frame_published = PublishEmbeddedInputFrame(*state);
        DrawPlatformUi();
        state->plugins->Draw(ImGui::GetCurrentContext());
        if (input_frame_published) PublishEmbeddedUiCapture(*state);
    }
    if (probing) {
        const ImVec2 origin = ImGui::GetMainViewport()->Pos;
        auto* marker = ImGui::GetForegroundDrawList();
        marker->AddRectFilled(
            ImVec2(origin.x + 2.0F, origin.y + 2.0F),
            ImVec2(origin.x + 30.0F, origin.y + 30.0F),
            IM_COL32(255, 0, 255, 255));
        marker->AddRectFilled(
            ImVec2(origin.x + 16.0F, origin.y + 2.0F),
            ImVec2(origin.x + 30.0F, origin.y + 30.0F),
            IM_COL32(0, 255, 255, 255));
    }
    ImGui::Render();
    NotifyRenderThread(*state, capture_generation);

    if (probing) {
        auto to_copy = Transition(
            frame.back_buffer, D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        state->command_list->ResourceBarrier(1, &to_copy);
        CopyBackBuffer(
            state->command_list, frame.back_buffer,
            state->pixel_probe.before_overlay, state->pixel_probe.footprint);
        auto to_render_target = Transition(
            frame.back_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        state->command_list->ResourceBarrier(1, &to_render_target);
    } else {
        auto to_render_target = Transition(
            frame.back_buffer, D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        state->command_list->ResourceBarrier(1, &to_render_target);
    }
    state->command_list->OMSetRenderTargets(1, &frame.render_target, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[]{state->shader_heap};
    state->command_list->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), state->command_list);
    if (probing) {
        auto to_copy = Transition(
            frame.back_buffer, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        state->command_list->ResourceBarrier(1, &to_copy);
        CopyBackBuffer(
            state->command_list, frame.back_buffer,
            state->pixel_probe.after_overlay, state->pixel_probe.footprint);
        auto to_present = Transition(
            frame.back_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_PRESENT);
        state->command_list->ResourceBarrier(1, &to_present);
    } else {
        auto to_present = Transition(
            frame.back_buffer, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT);
        state->command_list->ResourceBarrier(1, &to_present);
    }
    if (FAILED(state->command_list->Close())) {
        if (probing) ReleasePixelProbeResources(state->pixel_probe);
        static_cast<void>(ReleaseGraphics(*state, RendererLifecycle::DeviceLost));
        return;
    }
    ID3D12CommandList* command_lists[]{state->command_list};
    ID3D12CommandQueue* queue = state->render_queue;
    if (queue == nullptr) {
        if (probing) ReleasePixelProbeResources(state->pixel_probe);
        static_cast<void>(ReleaseGraphics(*state, RendererLifecycle::DeviceLost));
        return;
    }
    queue->ExecuteCommandLists(1, command_lists);
    const UINT64 signal_value = state->next_fence_value++;
    frame.fence_value = signal_value;
    if (probing) {
        state->pixel_probe.command_recorded = false;
        state->pixel_probe.pending = true;
        state->pixel_probe.fence_value = signal_value;
    }
    if (FAILED(queue->Signal(state->fence, signal_value))) {
        state->submission_unfenced = true;
        state->renderer = RendererLifecycle::DeviceLost;
        return;
    }
    }
    // Lifecycle mutations are submitted only after the render mutex and the
    // plugin draw lock have been released, so a slow reload cannot block
    // Resize/Present teardown.
    FlushPlatformUiActions();
}

bool BeforeResize(
    IDXGISwapChain* swap_chain,
    UINT present_queue_count,
    IUnknown* const* present_queues,
    bool validate_present_queues) {
    auto* state = g_state.load(std::memory_order_acquire);
    if (state == nullptr) return false;
    std::scoped_lock lock(state->render_mutex);
    if (state->source_swap_chain != swap_chain ||
        state->renderer.load() != RendererLifecycle::Ready) return false;
    SelectPendingResizeQueue(
        *state, present_queue_count, present_queues, validate_present_queues);
    state->renderer = RendererLifecycle::ResizePending;
    if (!ReleaseBackBuffersForResize(*state)) {
        // An invalid ResizeBuffers1 queue set is terminal for this renderer
        // generation. Keep the validation result for AfterResize so it can
        // retire the generation even when the underlying DXGI call fails.
        if (!state->pending_resize_queue_valid) return true;
        ClearPendingResizeQueue(*state);
        state->renderer = RendererLifecycle::Ready;
        return false;
    }
    return true;
}

bool AfterResize(bool affected, HRESULT result) {
    if (!affected) return false;
    auto* state = g_state.load(std::memory_order_acquire);
    if (state == nullptr) return false;
    std::scoped_lock lock(state->render_mutex);
    if (!state->pending_resize_queue_valid) {
        ClearPendingResizeQueue(*state);
        static_cast<void>(ReleaseGraphics(*state, RendererLifecycle::Stopped));
        return false;
    }
    bool queue_changed{};
    if (SUCCEEDED(result)) {
        const ResizeQueueAdoption queue = AdoptPendingResizeQueue(*state);
        if (queue == ResizeQueueAdoption::Invalid) {
            static_cast<void>(ReleaseGraphics(*state, RendererLifecycle::Stopped));
            return false;
        }
        queue_changed = queue == ResizeQueueAdoption::Changed;
    } else {
        ClearPendingResizeQueue(*state);
    }
    if (RebuildBackBuffersAfterResize(*state, queue_changed)) {
        // Keep a successful Resize gated until the bridge has published its
        // callback. That establishes the coordinator's Resize epoch before a
        // post-Resize Present is allowed to complete the next pixel probe.
        if (FAILED(result)) state->renderer = RendererLifecycle::Ready;
        return SUCCEEDED(result);
    }
    static_cast<void>(ReleaseGraphics(
        *state, SUCCEEDED(result) ? RendererLifecycle::Cold : RendererLifecycle::DeviceLost));
    return false;
}

void FinishResizeEvidenceHandoff(bool successful) noexcept {
    if (!successful) return;
    auto* state = g_state.load(std::memory_order_acquire);
    if (state == nullptr) return;
    try {
        std::scoped_lock lock(state->render_mutex);
        if (state->renderer.load() != RendererLifecycle::ResizePending) return;
        // A successful Resize starts a new pixel-evidence epoch. The next
        // Present must perform another before/after overlay readback even
        // when this capture generation had already reported pixels.
        RearmSmokeProbeAfterResize(state->pixel_probe.policy);
        state->renderer = RendererLifecycle::Ready;
    } catch (...) {
        state->renderer = RendererLifecycle::DeviceLost;
    }
}

void CaptureCommandQueue(ID3D12CommandQueue* queue) {
    auto* state = g_state.load(std::memory_order_acquire);
    if (state == nullptr || queue == nullptr ||
        queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    std::scoped_lock lock(state->queue_mutex);
    if (state->captured_queue == queue) return;
    queue->AddRef();
    Release(state->captured_queue);
    state->captured_queue = queue;
}

void HandlePresentResult(IDXGISwapChain* swap_chain, HRESULT result) {
    if (result != DXGI_ERROR_DEVICE_REMOVED && result != DXGI_ERROR_DEVICE_RESET) return;
    auto* state = g_state.load(std::memory_order_acquire);
    if (state == nullptr) return;
    std::scoped_lock lock(state->render_mutex);
    if (state->source_swap_chain == swap_chain) {
        static_cast<void>(ReleaseGraphics(*state, RendererLifecycle::DeviceLost));
    }
}

}  // namespace ue5mem::embedded
