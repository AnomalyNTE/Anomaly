#pragma once

#include "platform_host.hpp"
#include "embedded_d3d12_interop.hpp"
#include "embedded_ime.hpp"
#include "shader_resource_descriptor_allocator.hpp"
#include "smoke_probe_policy.hpp"

#include "anomaly/hook_manager.hpp"
#include "anomaly/input_service.hpp"
#include "anomaly/ui_service_registry.hpp"
#include "plugin_manager.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>


namespace anomaly::frame_aux {
inline constexpr std::size_t kKeyDigits = 12;
inline constexpr std::size_t kPayloadBytes = 86;
inline constexpr std::size_t kBlockCells = 46;
inline constexpr std::size_t kBlockSide = kBlockCells * 8;
using Payload = std::array<std::uint8_t, kPayloadBytes>;
inline constexpr std::size_t kBlockBytes = kBlockSide * kBlockSide * sizeof(float);
using Block = std::vector<float>;

// Lifecycle/Worker only. Base-mode HPKE (RFC 9180): P-384/SHA384/AES256GCM.
// recipient is SEC1 uncompressed (97 bytes). The private key never enters Core.
[[nodiscard]] std::optional<Payload> Wrap(std::string_view key, std::uint32_t unix_seconds,
                                         std::span<const std::uint8_t> recipient) noexcept;
[[nodiscard]] Block BuildBlock(const Payload& payload);

// Owns the GPU resources for the per-frame auxiliary payload. Never invokes plugins, game objects
// or I/O from recording/submission hooks. All submitted lists restore PRESENT state.
class Channel final {
   public:
    struct Diagnostic {
        const char* operation{"not-initialized"};
        HRESULT result{S_OK};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        D3D12_RESOURCE_DESC buffer{};
        UINT buffer_count{};
    };
    void Prepare() noexcept;  // Lifecycle, shader compilation only.
    // Resolution of the sealed wall clock. The frame carries the seal time, so the
    // payload is re-sealed once per bucket instead of once per session.
    static constexpr std::int64_t kResealSeconds = 10;
    // Worker supplies an owned, current key plus the wall clock; nullopt disables
    // the payload.
    void UpdateKey(const std::optional<std::string>& key, std::int64_t unix_seconds) noexcept;
    bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue,
                    std::span<ID3D12Resource* const> buffers, DXGI_FORMAT format) noexcept;
    void Observe(ID3D12GraphicsCommandList* list, UINT count,
                 const D3D12_RESOURCE_BARRIER* barriers) noexcept;
    void ResetList(ID3D12GraphicsCommandList* list) noexcept;
    // Serializes original + auxiliary submission on the selected direct queue,
    // so another capture submission cannot slip between the two.
    void Execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists,
                 void(STDMETHODCALLTYPE* execute)(ID3D12CommandQueue*, UINT,
                                                  ID3D12CommandList* const*),
                 bool embed = true) noexcept;
    // Only Resize/stop may wait. false retains resources until a later drain.
    bool Release(DWORD timeout_ms = 0) noexcept;
    [[nodiscard]] bool Prepared() const noexcept {
        return !compute_.empty();
    }
    std::atomic_uint64_t submitted{}, skipped_busy{}, failed{};

   private:
    void Fail(const char* operation, HRESULT result) noexcept;
    template <class T>
    using Com = Microsoft::WRL::ComPtr<T>;
    struct Frame {
        Com<ID3D12Resource> buffer, output;
        Com<ID3D12CommandAllocator> allocator;
        Com<ID3D12GraphicsCommandList> list;
        UINT64 fence{};
        Com<ID3D12Resource> aux, aux_upload;
        UINT64 aux_generation{};
    };
    std::atomic_bool active_{};
    std::atomic<ID3D12CommandQueue*> active_queue_{};
    std::mutex mutex_;
    Diagnostic diagnostic_;
    std::unique_ptr<std::array<float, kBlockCells * kBlockCells + 64>> block_data_;
    std::string current_key_;  // Worker only.
    std::int64_t current_bucket_{INT64_MIN};  // Worker only.
    UINT64 payload_generation_{};
    std::vector<std::uint8_t> compute_;
    Com<ID3D12Device> device_;
    Com<ID3D12CommandQueue> queue_;
    Com<ID3D12DescriptorHeap> srv_;
    Com<ID3D12RootSignature> root_;
    Com<ID3D12PipelineState> pipeline_;
    Com<ID3D12Fence> fence_;
    std::vector<Frame> frames_;
    UINT width_{}, height_{}, srv_increment_{}, pixel_format_{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    UINT64 epoch_{}, next_fence_{1};
    bool unfenced_{};
    bool initialization_attempted_{};
};
}  // namespace anomaly::frame_aux

struct ImGuiContext;

namespace ue5mem::embedded {

enum class EmbeddedPerformanceStage : std::uint8_t {
    PresentInterval,
    PresentLease,
    PresentRender,
    PresentOriginal,
    PresentTail,
    ExecuteLease,
    ExecuteCapture,
    ExecuteOriginal,
    RenderLockWait,
    RenderSetup,
    RenderFenceWait,
    RenderFrameReset,
    RenderUi,
    RenderPrepareLockWait,
    RenderPrepareLocked,
    RenderFrameBegin,
    RenderDrawLockWait,
    RenderInput,
    RenderPlatformUi,
    RenderPluginDraw,
    RenderCapture,
    RenderFinalize,
    RenderCommands,
    RenderSubmit,
    RenderTotal,
    GamePump,
    GamePluginLockWait,
    GamePluginUpdate,
    GameEscMenu,
    GameTotal,
    GameTickInterval,
    WorkerPluginLockWait,
    WorkerPluginMaintenance,
    WorkerRetryServices,
    WorkerPollChanges,
    WorkerMaintenance,
    WorkerPersist,
    PlatformUiSubmissionLockWait,
    PlatformUiOperationLockWait,
    PlatformUiWindowState,
    PlatformUiRefreshCatalog,
    PlatformUiRuntimePlugins,
    PlatformUiBuildSnapshot,
    PlatformUiRepositorySnapshot,
    PlatformUiServiceGraphSnapshot,
    PlatformUiAdapterServicesSnapshot,
    PlatformUiNteCompatibilitySnapshot,
    PlatformUiModelPublish,
    PlatformUiSettingsRefresh,
    PlatformUiRefreshTotal,
    PlatformUiFrameSetup,
    PlatformUiManagementShell,
    PlatformUiPopups,
    PlatformUiWindowPersist,
    Count,
};

struct EmbeddedPerformanceBucket {
    std::atomic_uint64_t samples{};
    std::atomic_uint64_t total_nanoseconds{};
    std::atomic_uint64_t maximum_nanoseconds{};
    std::atomic_uint64_t window_samples{};
    std::atomic_uint64_t window_total_nanoseconds{};
    std::atomic_uint64_t window_maximum_nanoseconds{};
};

class EmbeddedPerformanceProbe final {
public:
    void SetEnabled(bool enabled) noexcept;
    [[nodiscard]] bool Enabled() const noexcept;
    void ObservePresent() noexcept;
    void ObserveExecute() noexcept;
    [[nodiscard]] bool SampleRender() noexcept;
    [[nodiscard]] bool SampleGameTick() noexcept;
    void Record(
        EmbeddedPerformanceStage stage,
        std::chrono::steady_clock::duration elapsed) noexcept;
    void RecordMaintenance(std::chrono::steady_clock::duration elapsed) noexcept;
    void RecordPersistence(std::chrono::steady_clock::duration elapsed) noexcept;
    void Publish(const std::shared_ptr<anomaly::StructuredLogger>& logger) noexcept;

private:
    std::array<
        EmbeddedPerformanceBucket,
        static_cast<std::size_t>(EmbeddedPerformanceStage::Count)> buckets_;
    std::atomic_uint64_t present_calls_{};
    std::atomic_uint64_t execute_calls_{};
    std::atomic_uint64_t render_calls_{};
    std::atomic_uint64_t game_tick_calls_{};
    std::atomic_uint64_t maintenance_calls_{};
    std::atomic_uint64_t persistence_calls_{};
    std::atomic_uint64_t present_gap_over_25ms_{};
    std::atomic_uint64_t present_gap_over_50ms_{};
    std::atomic_int64_t last_present_nanoseconds_{};
    std::atomic_int64_t last_game_tick_nanoseconds_{};
    std::atomic_bool enabled_{};
    std::atomic_bool reset_requested_{};
    std::chrono::steady_clock::time_point last_publish_{};
};

template <typename T>
void Release(T*& value) {
    if (value != nullptr) {
        value->Release();
        value = nullptr;
    }
}

struct FrameContext {
    ID3D12CommandAllocator* allocator{};
    ID3D12Resource* back_buffer{};
    D3D12_CPU_DESCRIPTOR_HANDLE render_target{};
    UINT64 fence_value{};
};

struct PixelProbeState {
    ID3D12Resource* before_overlay{};
    ID3D12Resource* after_overlay{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT row_count{};
    UINT64 row_size_bytes{};
    UINT64 total_bytes{};
    UINT64 fence_value{};
    SmokeProbePolicy policy;
    SmokeProbeTicket ticket;
    bool command_recorded{};
    bool pending{};
};

// WndProc only records host input here. Render consumes the mailbox after
// ImGui::NewFrame, which keeps hotkey dispatch and PluginManager access out of
// the window-procedure thread.
struct EmbeddedInputMailbox {
    std::mutex mutex;
    anomaly::InputFrameState frame;
    float last_published_mouse_x{};
    float last_published_mouse_y{};
    std::int64_t pending_wheel_delta{};
    bool mouse_position_published{};
    bool reset_pending{};
    anomaly::InputResetReason reset_reason{anomaly::InputResetReason::None};
};

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
    const UINT*, IUnknown* const*);
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(
    ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using CreateSwapChainForCoreWindowFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
    IDXGIOutput*, IDXGISwapChain1**);

enum class RendererLifecycle : std::uint8_t {
    Cold,
    Discovering,
    Ready,
    ResizePending,
    DeviceLost,
    Stopping,
    Stopped,
};

// Which Direct3D version the intercepted swap chain belongs to.
//
// The Present hook lives in DXGI, which every Direct3D version shares, so the
// overlay gets called for a D3D11 title exactly as it does for a D3D12 one and
// has to draw with whichever API the back buffer actually belongs to.
enum class EmbeddedRenderApi : std::uint8_t { None, D3D12, D3D11 };

struct HookTargets {
    void* resource_barrier{};
    void* reset_command_list{};
    void* create_swap_chain{};
    void* create_swap_chain_for_hwnd{};
    void* create_swap_chain_for_core_window{};
    void* execute_command_lists{};
    void* present{};
    void* present1{};
    void* resize_buffers{};
    void* resize_buffers1{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return create_swap_chain != nullptr && create_swap_chain_for_hwnd != nullptr &&
            create_swap_chain_for_core_window != nullptr &&
            execute_command_lists != nullptr && present != nullptr &&
            present1 != nullptr && resize_buffers != nullptr && resize_buffers1 != nullptr;
    }
};

// Reports what the submission target's first bytes currently look like and,
// when they are a jump, which module it lands in. Distinguishes a detour that
// is still ours from one that was overwritten by another hook.
[[nodiscard]] std::string DescribeEmbeddedSubmissionTarget();

struct EmbeddedState {
    anomaly::frame_aux::Channel frame_aux;
    std::filesystem::path root;
    std::string imgui_ini_path;
    AnalyzerConfig config;
    PlatformDiagnostics diagnostics;
    EmbeddedPerformanceProbe performance;
    // Borrowed from the RuntimeSession composition root. A quarantined UI
    // owner carries its own shared lifetime token; the renderer state itself
    // must not become a second PluginManager owner.
    PluginManager* plugins{};
    std::weak_ptr<PluginManager> plugin_owner;
    // Populated only when the entire renderer generation is quarantined after
    // a hook/UI deadline. Normal renderer operation remains composition-root
    // borrowed.
    std::shared_ptr<PluginManager> quarantined_plugin_owner;
    std::mutex render_mutex;
    std::shared_ptr<std::mutex> plugin_mutex{std::make_shared<std::mutex>()};
    std::mutex queue_mutex;
    EmbeddedQueueCapture queue_capture;
    ID3D12CommandQueue* render_queue{};
    ID3D12CommandQueue* pending_resize_queue{};
    bool pending_resize_queue_valid{true};
    IDXGISwapChain* source_swap_chain{};
    IDXGISwapChain3* swap_chain{};
    ID3D12Device* device{};
    ID3D12DescriptorHeap* render_target_heap{};
    ID3D12DescriptorHeap* shader_heap{};
    ShaderResourceDescriptorAllocator shader_descriptors;
    ID3D12GraphicsCommandList* command_list{};
    ID3D12Fence* fence{};
    HANDLE fence_event{};
    std::vector<FrameContext> frames;
    // Set once the swap chain's API is known, and the switch every per-frame
    // path branches on. D3D11 needs none of the queue, fence, allocator or
    // descriptor-heap machinery above it: the immediate context is the whole
    // submission model.
    EmbeddedRenderApi render_api{EmbeddedRenderApi::None};
    ID3D11Device* d3d11_device{};
    ID3D11DeviceContext* d3d11_context{};
    ID3D11RenderTargetView* d3d11_render_target{};
    bool dx11_initialized{};
    DXGI_FORMAT render_target_format{DXGI_FORMAT_UNKNOWN};
    UINT64 next_fence_value{1};
    bool submission_unfenced{};
    PixelProbeState pixel_probe;
    HWND window{};
    ImGuiContext* imgui_context{};
    WNDPROC original_window_proc{};
    EmbeddedImeContext ime;
    // Set when the window procedure could not be exchanged. The overlay still
    // renders in that case, so the reason has to survive for diagnostics.
    unsigned long input_install_error{};
    bool input_installed{};
    // Evidence about the command-queue handover, which only a D3D12 title
    // performs. The overlay submits onto the game's own direct queue so its
    // draws stay ordered against the game's frame, which means a missing queue
    // stops the overlay entirely. These counters separate "the submission hook
    // never fires" from "it fires but never with a direct queue" in a log.
    std::atomic<std::uint64_t> execute_hook_calls{};
    std::atomic<std::uint64_t> execute_direct_queues{};
    std::atomic<std::uint32_t> observed_queue_types{};
    HWND previous_capture{};
    RECT previous_cursor_clip{};
    bool cursor_clip_saved{};
    bool menu_cursor_active{};
    bool win32_initialized{};
    bool dx12_initialized{};
    bool platform_ui_initialized{};
    bool plugin_ui_device_active{};
    bool lifecycle_invoker_bound{};
    bool plugins_loaded{};
    std::atomic<RendererLifecycle> renderer{RendererLifecycle::Cold};
    std::uint64_t selected_area{};
    anomaly::UiServiceRegistry ui_services;
    EmbeddedInputMailbox input_mailbox;
};

extern std::atomic<EmbeddedState*> g_state;
extern PresentFn g_present;
extern Present1Fn g_present1;
extern ResizeBuffersFn g_resize_buffers;
extern ResizeBuffers1Fn g_resize_buffers1;
extern ExecuteCommandListsFn g_execute_command_lists;
extern std::unique_ptr<anomaly::HookManager> g_hooks;
inline constexpr std::string_view kRendererHookOwner = "anomaly.renderer";
inline constexpr std::uint64_t kRendererHookGeneration = 1;

[[nodiscard]] HookTargets DiscoverD3D12HookTargets();

[[nodiscard]] bool InstallEmbeddedInput(EmbeddedState& state) noexcept;
[[nodiscard]] bool RestoreEmbeddedInput(EmbeddedState& state) noexcept;
void RecordEmbeddedInputMessage(
    EmbeddedState& state, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
// These are called on Render while the existing PluginManager mutex is held.
// The mailbox keeps their input collection independent from WndProc.
[[nodiscard]] bool PublishEmbeddedInputFrame(EmbeddedState& state) noexcept;
void PublishEmbeddedUiCapture(EmbeddedState& state) noexcept;

[[nodiscard]] const AnomalyUiServiceV1* EmbeddedUiServiceTable() noexcept;

void RenderEmbedded(IDXGISwapChain* swap_chain, UINT flags);
[[nodiscard]] bool BeforeResize(
    IDXGISwapChain* swap_chain,
    UINT present_queue_count = 0,
    IUnknown* const* present_queues = nullptr,
    bool validate_present_queues = false);
[[nodiscard]] bool AfterResize(bool affected, HRESULT result);
void FinishResizeEvidenceHandoff(bool successful) noexcept;
void CaptureCommandQueue(ID3D12CommandQueue* queue);
void HandlePresentResult(IDXGISwapChain* swap_chain, HRESULT result);
[[nodiscard]] bool ReleaseGraphics(
    EmbeddedState& state, RendererLifecycle final_state, bool force_release = false);

}  // namespace ue5mem::embedded
