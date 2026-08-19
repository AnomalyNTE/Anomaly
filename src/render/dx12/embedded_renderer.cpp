#include "embedded_host_internal.hpp"
#include "anomaly/host_ui_service.hpp"
#include "anomaly/platform_ui_input_policy.hpp"
#include "anomaly/platform_ui_theme.hpp"
#include "anomaly/structured_logger.hpp"

#include "embedded_ui_resource_render_backend.hpp"

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <array>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <utility>

namespace ue5mem::embedded {
namespace {

// Says why the overlay is not on screen.
//
// Embedded start-up used to be entirely silent: a dozen separate conditions
// could each abandon the frame with a bare `return`, so a user whose overlay
// never appeared had nothing to go on and neither did anyone reading a bug
// report. Every abandoned start-up now names its reason exactly once per
// reason, which keeps a per-frame path from turning into a log flood while
// still surfacing the first cause and any later change of cause.
void ReportEmbeddedGate(
        const EmbeddedState& state, const char* reason, const std::string& detail = {},
        const char* prefix = "embedded overlay not started: ") noexcept {
    try {
        static std::mutex mutex;
        static std::set<std::string> reported;
        const std::string key = std::string(reason) + '|' + detail;
        {
            std::scoped_lock lock(mutex);
            if (!reported.insert(key).second) return;
        }
        std::string message = std::string(prefix) + reason;
        if (!detail.empty()) message += " (" + detail + ")";
        std::ofstream(state.root / L"anomaly-platform.log", std::ios::app)
            << "pid=" << GetCurrentProcessId() << ' ' << message << std::endl;
        if (state.diagnostics.logger != nullptr) {
            anomaly::LogDetails details;
            details.thread_domain = anomaly::LogThreadDomain::Render;
            details.event_id = "embedded.gate";
            static_cast<void>(state.diagnostics.logger->Log(
                anomaly::LogLevel::Warning, "embedded-renderer", message, std::move(details)));
        }
    } catch (...) {
    }
}

// Drives the overlay from polled device state.
//
// Used only when the game window's procedure could not be exchanged. ImGui
// normally learns about input from window messages, so without that hook the
// overlay would draw but ignore the mouse entirely, which is barely better than
// not drawing at all. Polling is coarser -- no character input, no scroll
// accumulation between frames -- but it restores pointing and clicking, which is
// what the panels actually need.
void FeedPolledInput(const EmbeddedState& state) noexcept {
    if (state.window == nullptr) return;
    ImGuiIO& io = ImGui::GetIO();
    POINT cursor{};
    if (GetCursorPos(&cursor) && ScreenToClient(state.window, &cursor)) {
        io.AddMousePosEvent(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
    }
    const bool foreground = GetForegroundWindow() == state.window;
    io.AddFocusEvent(foreground);
    if (!foreground) return;
    struct PolledButton {
        int virtual_key;
        int imgui_button;
    };
    static constexpr std::array<PolledButton, 3> buttons{{
        {VK_LBUTTON, 0}, {VK_RBUTTON, 1}, {VK_MBUTTON, 2}}};
    for (const PolledButton& button : buttons) {
        const bool down = (GetAsyncKeyState(button.virtual_key) & 0x8000) != 0;
        io.AddMouseButtonEvent(button.imgui_button, down);
    }
}

std::string LastErrorDetail(const char* label, unsigned long error) {
    return std::string(label) + "=" + std::to_string(error);
}


class PerformanceTimer final {
public:
    PerformanceTimer(
        EmbeddedPerformanceProbe& probe,
        const EmbeddedPerformanceStage stage,
        const bool active) noexcept
        : probe_(active ? &probe : nullptr), stage_(stage),
          started_(active ? std::chrono::steady_clock::now()
                          : std::chrono::steady_clock::time_point{}) {}

    ~PerformanceTimer() { Stop(); }

    PerformanceTimer(const PerformanceTimer&) = delete;
    PerformanceTimer& operator=(const PerformanceTimer&) = delete;

    void Stop() noexcept {
        if (probe_ == nullptr) return;
        probe_->Record(stage_, std::chrono::steady_clock::now() - started_);
        probe_ = nullptr;
    }

private:
    EmbeddedPerformanceProbe* probe_{};
    EmbeddedPerformanceStage stage_{};
    std::chrono::steady_clock::time_point started_{};
};

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
    // ResizeBuffers fails while anything still references a back buffer, and on
    // D3D11 the overlay's render target view is that reference.
    Release(state.d3d11_render_target);
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
    if (state.render_api == EmbeddedRenderApi::D3D11) {
        // Only the render target view needs rebuilding; there is no descriptor
        // heap and no per-frame context to reconfigure.
        if (state.source_swap_chain == nullptr || state.d3d11_device == nullptr) return false;
        ID3D11Texture2D* back_buffer{};
        if (FAILED(state.source_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))) ||
            back_buffer == nullptr) {
            Release(back_buffer);
            return false;
        }
        const HRESULT created = state.d3d11_device->CreateRenderTargetView(
            back_buffer, nullptr, &state.d3d11_render_target);
        Release(back_buffer);
        if (FAILED(created) || state.d3d11_render_target == nullptr) return false;
        RECT output_rect{};
        if (state.window != nullptr && GetClientRect(state.window, &output_rect)) {
            const LONG width = (std::max)(0L, output_rect.right - output_rect.left);
            const LONG height = (std::max)(0L, output_rect.bottom - output_rect.top);
            state.selected_area =
                static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
        }
        return true;
    }
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

// Names the graphics API behind a swap chain.
//
// The Present hook lives in DXGI, which every Direct3D version shares, so it
// fires for a D3D11 swap chain exactly as it does for a D3D12 one. Everything
// downstream of it here is D3D12 only. When the device query fails there is no
// way to tell "wrong API" from "device query failed" without asking, so ask.
std::string DescribeSwapChainApi(IDXGISwapChain* swap_chain) {
    if (swap_chain == nullptr) return "swapChain=null";
    std::string description = "api=";
    void* probe{};
    const auto query = [&](const IID& id) {
        probe = nullptr;
        return SUCCEEDED(swap_chain->GetDevice(id, &probe)) && probe != nullptr;
    };
    if (query(__uuidof(ID3D12Device))) {
        description += "d3d12";
    } else if (query(IID{0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}})) {
        // ID3D11Device, spelled out so this file does not have to pull in d3d11.h.
        description += "d3d11";
    } else if (query(IID{0x9b7e4c0f, 0x342c, 0x4106, {0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0}})) {
        // ID3D10Device1.
        description += "d3d10";
    } else {
        description += "unknown";
    }
    if (probe != nullptr) static_cast<IUnknown*>(probe)->Release();
    DXGI_SWAP_CHAIN_DESC layout{};
    if (SUCCEEDED(swap_chain->GetDesc(&layout))) {
        description += " buffers=" + std::to_string(layout.BufferCount) +
            " format=" + std::to_string(static_cast<int>(layout.BufferDesc.Format)) +
            " windowed=" + std::to_string(layout.Windowed ? 1 : 0);
    }
    return description;
}

bool CompleteGraphicsInitialization(EmbeddedState& state);

bool InitializeGraphicsD3D12(EmbeddedState& state, IDXGISwapChain* swap_chain) {
    if (state.plugins == nullptr || state.quarantined_plugin_owner != nullptr ||
        PlatformUiQuarantined(state.plugins)) {
        // A retained owner is a generation fence. Do not allocate another
        // ImGui/device generation until the prior UI callbacks have drained.
        ReportEmbeddedGate(state, "plugin host unavailable or quarantined");
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
        // Normal for the first frames, permanent if the submission hook never
        // delivers. Repeat with the live evidence so the two cases can be told
        // apart: the counters say whether the hook ran at all, the target
        // description says whether the detour is still the one installed here,
        // and the swap chain description says which API it belongs to.
        static std::atomic<long long> next_report{};
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        long long expected = next_report.load(std::memory_order_relaxed);
        if (now >= expected &&
            next_report.compare_exchange_strong(
                expected, now + std::chrono::steady_clock::duration(
                    std::chrono::seconds(5)).count())) {
            std::ostringstream detail;
            detail << "submissionHookCalls="
                   << state.execute_hook_calls.load(std::memory_order_relaxed)
                   << " directQueues="
                   << state.execute_direct_queues.load(std::memory_order_relaxed)
                   << " queueTypeMask=0x" << std::hex
                   << state.observed_queue_types.load(std::memory_order_relaxed) << std::dec
                   << ' ' << DescribeEmbeddedSubmissionTarget()
                   << ' ' << DescribeSwapChainApi(swap_chain);
            ReportEmbeddedGate(state, "no direct command queue captured yet", detail.str());
        }
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
        ReportEmbeddedGate(
            state, "swap chain is not a usable D3D12 target "
            "(no IDXGISwapChain3, no ID3D12Device, or no output window)");
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
        ReportEmbeddedGate(
            state, "output window rejected",
            "process=" + std::to_string(output_process) +
                " visible=" + std::to_string(IsWindowVisible(description.OutputWindow) ? 1 : 0) +
                " size=" + std::to_string(width) + "x" + std::to_string(height));
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
    state.render_api = EmbeddedRenderApi::D3D12;
    return CompleteGraphicsInitialization(state);
}

// Everything after the device objects exist, which is identical whichever
// Direct3D version owns the back buffer. The resource backend serves both, so
// plugin fonts and icons work the same way on each.
bool CompleteGraphicsInitialization(EmbeddedState& state) {
    if (!InstallEmbeddedInput(state)) {
        // Losing the window procedure costs interactivity, not the overlay.
        // Tearing the renderer down here is what turned a recoverable input
        // problem into a completely dead platform: no UI service reaches the
        // plugins, so nothing renders and the hotkey never runs either.
        ReportEmbeddedGate(
            state, "window procedure could not be hooked; overlay stays visible but "
            "will not accept mouse or keyboard",
            LastErrorDetail("error", state.input_install_error));
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
    // Also written to the plain platform log, which is where every abandoned
    // start-up above reports, so a successful one is visible in the same place.
    {
        std::ostringstream ready;
        ready << "api=" << (state.render_api == EmbeddedRenderApi::D3D11 ? "d3d11" : "d3d12")
              << " hwnd=0x" << std::hex << reinterpret_cast<std::uintptr_t>(state.window)
              << std::dec << " toggleKey=" << state.config.platform_toggle_key
              << " inputHooked=" << (state.input_installed ? 1 : 0);
        ReportEmbeddedGate(state, "ready", ready.str(), "embedded overlay: ");
    }
    return true;
}

// Binds a render target view to back buffer zero.
//
// Split out because the view has to be dropped before ResizeBuffers and rebuilt
// afterwards; holding a reference to a back buffer is what makes a resize fail.
bool CreateD3D11RenderTarget(EmbeddedState& state) {
    if (state.d3d11_device == nullptr || state.source_swap_chain == nullptr) return false;
    ID3D11Texture2D* back_buffer{};
    if (FAILED(state.source_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))) ||
        back_buffer == nullptr) {
        Release(back_buffer);
        return false;
    }
    const HRESULT created = state.d3d11_device->CreateRenderTargetView(
        back_buffer, nullptr, &state.d3d11_render_target);
    Release(back_buffer);
    return SUCCEEDED(created) && state.d3d11_render_target != nullptr;
}

void ReleaseD3D11RenderTarget(EmbeddedState& state) noexcept {
    Release(state.d3d11_render_target);
}

// Brings the overlay up on a swap chain owned by a D3D11 device.
//
// Far smaller than the D3D12 path because the immediate context is the entire
// submission model: no command queue to capture, no allocators, no fences, no
// resource barriers, and one render target view instead of a descriptor heap.
bool InitializeGraphicsD3D11(EmbeddedState& state, IDXGISwapChain* swap_chain) {
    if (state.plugins == nullptr || state.quarantined_plugin_owner != nullptr ||
        PlatformUiQuarantined(state.plugins)) {
        ReportEmbeddedGate(state, "plugin host unavailable or quarantined");
        return false;
    }
    if (state.platform_ui_initialized &&
        !ReleaseGraphics(state, RendererLifecycle::Cold)) {
        return false;
    }
    state.renderer = RendererLifecycle::Discovering;
    ID3D11Device* device{};
    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(swap_chain->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr ||
        FAILED(swap_chain->GetDesc(&description)) || description.BufferCount == 0 ||
        description.OutputWindow == nullptr) {
        Release(device);
        ReportEmbeddedGate(
            state, "swap chain is not a usable D3D11 target",
            DescribeSwapChainApi(swap_chain));
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
        Release(device);
        ReportEmbeddedGate(
            state, "output window rejected",
            "process=" + std::to_string(output_process) +
                " visible=" + std::to_string(IsWindowVisible(description.OutputWindow) ? 1 : 0) +
                " size=" + std::to_string(width) + "x" + std::to_string(height));
        state.renderer = RendererLifecycle::Cold;
        return false;
    }
    state.d3d11_device = device;
    device->GetImmediateContext(&state.d3d11_context);
    swap_chain->AddRef();
    state.source_swap_chain = swap_chain;
    state.window = description.OutputWindow;
    state.render_target_format = description.BufferDesc.Format;
    state.selected_area = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (state.d3d11_context == nullptr || !CreateD3D11RenderTarget(state)) {
        ReportEmbeddedGate(state, "could not bind a render target to the D3D11 back buffer");
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    IMGUI_CHECKVERSION();
    state.imgui_context = ImGui::CreateContext();
    if (state.imgui_context == nullptr) {
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    static_cast<void>(ConfigurePlatformUiFontAtlas(state.root));
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
    state.imgui_ini_path = (state.root / L"anomaly-imgui.ini").string();
    io.IniFilename = state.imgui_ini_path.c_str();
    if (!ImGui_ImplWin32_Init(state.window)) {
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    state.win32_initialized = true;
    if (!ImGui_ImplDX11_Init(state.d3d11_device, state.d3d11_context)) {
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    state.dx11_initialized = true;
    state.render_api = EmbeddedRenderApi::D3D11;
    return CompleteGraphicsInitialization(state);
}

bool InitializeGraphics(EmbeddedState& state, IDXGISwapChain* swap_chain) {
    if (swap_chain == nullptr) return false;
    // Asking the swap chain settles which API owns the back buffer. Guessing
    // wrong here is what left the overlay waiting forever for a command queue
    // that a D3D11 title never creates.
    ID3D12Device* probe{};
    const bool is_d3d12 =
        SUCCEEDED(swap_chain->GetDevice(IID_PPV_ARGS(&probe))) && probe != nullptr;
    Release(probe);
    return is_d3d12 ? InitializeGraphicsD3D12(state, swap_chain)
                    : InitializeGraphicsD3D11(state, swap_chain);
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
    if (state.dx11_initialized) {
        ImGui_ImplDX11_Shutdown();
        state.dx11_initialized = false;
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
    ReleaseD3D11RenderTarget(state);
    Release(state.d3d11_context);
    Release(state.d3d11_device);
    state.render_api = EmbeddedRenderApi::None;
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
    const bool sampled = state->performance.SampleRender();
    PerformanceTimer total_timer(
        state->performance, EmbeddedPerformanceStage::RenderTotal, sampled);
    {
    const auto lock_started = sampled ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    std::unique_lock render_lock(state->render_mutex);
    if (sampled) {
        state->performance.Record(
            EmbeddedPerformanceStage::RenderLockWait,
            std::chrono::steady_clock::now() - lock_started);
    }
    PerformanceTimer setup_timer(
        state->performance, EmbeddedPerformanceStage::RenderSetup, sampled);
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
        const bool expanding = anomaly::HostUiMenusCollapsed();
        anomaly::SetHostUiMenusCollapsed(!expanding);
        // The shell's own close button marks the management window closed, and
        // that state is persisted. Collapsing is not the same as closing, so a
        // hotkey that only flipped the collapse flag could never bring a closed
        // shell back -- not in that session and not in any later one, because
        // start-up reads the persisted state and collapses again. Reopening here
        // is what makes the hotkey a way back in.
        if (expanding) static_cast<void>(RevealPlatformUi());
    }
    static_cast<void>(ApplyHostUiManagementExpansionRequest());

    // D3D11 submits through the immediate context, so none of the back buffer
    // index, fence wait, allocator reset or pixel probe below applies to it.
    const bool immediate_mode = state->render_api == EmbeddedRenderApi::D3D11;
    FrameContext* frame_context{};
    std::uint64_t capture_generation{};
    bool probing = false;
    if (immediate_mode) {
        setup_timer.Stop();
    } else {
        const UINT index = state->swap_chain->GetCurrentBackBufferIndex();
        if (index >= state->frames.size()) return;
        frame_context = &state->frames[index];
        capture_generation = CaptureGeneration(*state);
        SynchronizeSmokeProbeCapture(state->pixel_probe.policy, capture_generation);
        static_cast<void>(CompletePixelProbe(*state, capture_generation));
        setup_timer.Stop();
        PerformanceTimer fence_timer(
            state->performance, EmbeddedPerformanceStage::RenderFenceWait, sampled);
        const bool frame_ready = WaitForFrame(*state, *frame_context);
        fence_timer.Stop();
        if (!frame_ready) return;
        PerformanceTimer reset_timer(
            state->performance, EmbeddedPerformanceStage::RenderFrameReset, sampled);
        if (FAILED(frame_context->allocator->Reset()) ||
            FAILED(state->command_list->Reset(frame_context->allocator, nullptr))) {
            static_cast<void>(ReleaseGraphics(*state, RendererLifecycle::DeviceLost));
            return;
        }
        probing = capture_generation != 0 &&
            PreparePixelProbe(*state, *frame_context, capture_generation);
        reset_timer.Stop();
    }

    PerformanceTimer ui_timer(
        state->performance, EmbeddedPerformanceStage::RenderUi, sampled);
    if (immediate_mode) {
        ImGui_ImplDX11_NewFrame();
    } else {
        ImGui_ImplDX12_NewFrame();
    }
    // NewFrame creates the ImGui font atlas first, reserving descriptor slot
    // zero before scoped texture uploads allocate from the shared heap.
    const auto prepare_lock_started = sampled ? std::chrono::steady_clock::now()
                                              : std::chrono::steady_clock::time_point{};
    {
        std::unique_lock plugin_lock(*state->plugin_mutex);
        const auto prepare_started = sampled ? std::chrono::steady_clock::now()
                                             : std::chrono::steady_clock::time_point{};
        if (sampled) {
            state->performance.Record(
                EmbeddedPerformanceStage::RenderPrepareLockWait,
                prepare_started - prepare_lock_started);
        }
        SynchronizeHostFontScale(*state);
        state->plugins->PrepareUiResources();
        PreparePlatformUiResources();
        if (sampled) {
            state->performance.Record(
                EmbeddedPerformanceStage::RenderPrepareLocked,
                std::chrono::steady_clock::now() - prepare_started);
        }
    }
    const auto frame_begin_started = sampled ? std::chrono::steady_clock::now()
                                             : std::chrono::steady_clock::time_point{};
    UpdateMenuCursor(*state, anomaly::HostUiMenusCaptureMouse());
    ImGui_ImplWin32_NewFrame();
    if (!state->input_installed) FeedPolledInput(*state);
    ImGui::NewFrame();
    anomaly::PrepareHostUiFrame();
    if (sampled) {
        state->performance.Record(
            EmbeddedPerformanceStage::RenderFrameBegin,
            std::chrono::steady_clock::now() - frame_begin_started);
    }
    bool input_frame_published{};
    const auto draw_lock_started = sampled ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
    {
        std::unique_lock plugin_lock(*state->plugin_mutex);
        auto phase_started = sampled ? std::chrono::steady_clock::now()
                                     : std::chrono::steady_clock::time_point{};
        if (sampled) {
            state->performance.Record(
                EmbeddedPerformanceStage::RenderDrawLockWait,
                phase_started - draw_lock_started);
        }
        input_frame_published = PublishEmbeddedInputFrame(*state);
        if (sampled) {
            const auto phase_completed = std::chrono::steady_clock::now();
            state->performance.Record(
                EmbeddedPerformanceStage::RenderInput,
                phase_completed - phase_started);
            phase_started = phase_completed;
        }
        DrawPlatformUi();
        if (sampled) {
            const auto phase_completed = std::chrono::steady_clock::now();
            state->performance.Record(
                EmbeddedPerformanceStage::RenderPlatformUi,
                phase_completed - phase_started);
            phase_started = phase_completed;
        }
        state->plugins->Draw(ImGui::GetCurrentContext());
        if (sampled) {
            const auto phase_completed = std::chrono::steady_clock::now();
            state->performance.Record(
                EmbeddedPerformanceStage::RenderPluginDraw,
                phase_completed - phase_started);
            phase_started = phase_completed;
        }
        if (input_frame_published) PublishEmbeddedUiCapture(*state);
        if (sampled) {
            state->performance.Record(
                EmbeddedPerformanceStage::RenderCapture,
                std::chrono::steady_clock::now() - phase_started);
        }
    }
    const auto finalize_started = sampled ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
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
    if (sampled) {
        state->performance.Record(
            EmbeddedPerformanceStage::RenderFinalize,
            std::chrono::steady_clock::now() - finalize_started);
    }
    ui_timer.Stop();

    if (immediate_mode) {
        PerformanceTimer command_timer(
            state->performance, EmbeddedPerformanceStage::RenderCommands, sampled);
        if (state->d3d11_context == nullptr || state->d3d11_render_target == nullptr) {
            static_cast<void>(ReleaseGraphics(*state, RendererLifecycle::DeviceLost));
            return;
        }
        // The game leaves its own targets bound. Binding the back buffer for the
        // overlay draw and nothing else is the whole of the D3D11 submission:
        // the immediate context is already ordered behind the game's work.
        //
        // ImGui's D3D11 backend restores whatever was bound when it was entered,
        // which by then is the overlay's own target, so the game's binding has
        // to be saved and put back here instead.
        ID3D11RenderTargetView* previous_targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* previous_depth{};
        state->d3d11_context->OMGetRenderTargets(
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, previous_targets, &previous_depth);
        ID3D11RenderTargetView* targets[]{state->d3d11_render_target};
        state->d3d11_context->OMSetRenderTargets(1, targets, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        state->d3d11_context->OMSetRenderTargets(
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, previous_targets, previous_depth);
        for (auto* previous : previous_targets) Release(previous);
        Release(previous_depth);
        command_timer.Stop();
    } else {
        auto& frame = *frame_context;
        PerformanceTimer command_timer(
            state->performance, EmbeddedPerformanceStage::RenderCommands, sampled);
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
    command_timer.Stop();
    PerformanceTimer submit_timer(
        state->performance, EmbeddedPerformanceStage::RenderSubmit, sampled);
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
    submit_timer.Stop();
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
    if (state == nullptr) return;
    state->execute_hook_calls.fetch_add(1, std::memory_order_relaxed);
    if (queue == nullptr) return;
    const D3D12_COMMAND_LIST_TYPE type = queue->GetDesc().Type;
    if (type >= 0 && type < 31) {
        state->observed_queue_types.fetch_or(
            1u << static_cast<unsigned>(type), std::memory_order_relaxed);
    }
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    state->execute_direct_queues.fetch_add(1, std::memory_order_relaxed);
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
