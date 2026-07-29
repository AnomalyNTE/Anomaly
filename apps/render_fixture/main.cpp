#include "anomaly/runtime_session.hpp"

#include <Windows.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr UINT kBufferCount = 2;
constexpr UINT kInitialWidth = 64;
constexpr UINT kInitialHeight = 64;
constexpr std::size_t kMaximumRenderCallbacksPerFrame = 64;
constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

using namespace std::chrono_literals;

template <typename T>
class ComPtr final {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] T* Get() const noexcept { return value_; }
    [[nodiscard]] T** Put() noexcept {
        Reset();
        return &value_;
    }
    [[nodiscard]] T* operator->() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }

    void Reset() noexcept {
        if (value_ != nullptr) {
            value_->Release();
            value_ = nullptr;
        }
    }

private:
    T* value_{};
};

class UniqueHandle final {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}
    ~UniqueHandle() { Reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : value_(std::exchange(other.value_, nullptr)) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HANDLE Get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

    void Reset(HANDLE value = nullptr) noexcept {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }

private:
    HANDLE value_{};
};

std::atomic_uint32_t g_fixture_window_class_references{};

class FixtureWindow final {
public:
    FixtureWindow() = default;
    ~FixtureWindow() { Reset(); }

    FixtureWindow(const FixtureWindow&) = delete;
    FixtureWindow& operator=(const FixtureWindow&) = delete;

    HRESULT Create(UINT width, UINT height) noexcept {
        instance_ = GetModuleHandleW(nullptr);
        if (instance_ == nullptr) return HRESULT_FROM_WIN32(GetLastError());

        if (g_fixture_window_class_references.fetch_add(1, std::memory_order_acq_rel) == 0) {
            WNDCLASSEXW window_class{};
            window_class.cbSize = sizeof(window_class);
            window_class.hInstance = instance_;
            window_class.lpfnWndProc = WindowProc;
            window_class.lpszClassName = kClassName;
            if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                const HRESULT error = HRESULT_FROM_WIN32(GetLastError());
                static_cast<void>(g_fixture_window_class_references.fetch_sub(
                    1, std::memory_order_acq_rel));
                instance_ = nullptr;
                return error;
            }
        }
        class_reference_acquired_ = true;

        window_ = CreateWindowExW(
            0,
            kClassName,
            L"Anomaly D3D12 Render Fixture",
            WS_POPUP,
            0,
            0,
            static_cast<int>(width),
            static_cast<int>(height),
            nullptr,
            nullptr,
            instance_,
            this);
        if (window_ == nullptr) {
            const HRESULT error = HRESULT_FROM_WIN32(GetLastError());
            Reset();
            return error;
        }
        return S_OK;
    }

    [[nodiscard]] HWND Get() const noexcept { return window_; }
    [[nodiscard]] UINT DpiChangeCount() const noexcept { return dpi_change_count_; }
    [[nodiscard]] UINT LastDpi() const noexcept { return last_dpi_; }
    [[nodiscard]] bool IsMinimized() const noexcept {
        return minimized_ && window_ != nullptr && IsIconic(window_) != FALSE;
    }

    HRESULT Resize(UINT width, UINT height) const noexcept {
        if (SetWindowPos(
                window_, nullptr, 0, 0, static_cast<int>(width), static_cast<int>(height),
                SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER) == FALSE) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        return S_OK;
    }

    HRESULT Recreate(UINT width, UINT height) noexcept {
        Reset();
        return Create(width, height);
    }

    HRESULT Minimize() const noexcept {
        ShowWindow(window_, SW_MINIMIZE);
        return S_OK;
    }

    HRESULT Restore() const noexcept {
        ShowWindow(window_, SW_RESTORE);
        return S_OK;
    }

    HRESULT SimulateDpiChange(UINT dpi) noexcept {
        if (window_ == nullptr || dpi == 0) return E_INVALIDARG;
        RECT suggested{};
        if (GetWindowRect(window_, &suggested) == FALSE) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        const UINT previous_count = dpi_change_count_;
        static_cast<void>(SendMessageW(
            window_, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&suggested)));
        return dpi_change_count_ == previous_count + 1 && last_dpi_ == dpi ? S_OK : E_UNEXPECTED;
    }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            if (create != nullptr) {
                static_cast<void>(SetWindowLongPtrW(
                    window, GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(create->lpCreateParams)));
            }
        }
        auto* const self = reinterpret_cast<FixtureWindow*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        return self == nullptr ? DefWindowProcW(window, message, wparam, lparam)
                               : self->HandleMessage(window, message, wparam, lparam);
    }

    LRESULT HandleMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        switch (message) {
        case WM_SIZE:
            minimized_ = wparam == SIZE_MINIMIZED;
            break;
        case WM_DPICHANGED: {
            last_dpi_ = HIWORD(wparam);
            ++dpi_change_count_;
            const auto* const suggested = reinterpret_cast<const RECT*>(lparam);
            if (suggested != nullptr) {
                static_cast<void>(SetWindowPos(
                    window, nullptr, suggested->left, suggested->top,
                    suggested->right - suggested->left, suggested->bottom - suggested->top,
                    SWP_NOACTIVATE | SWP_NOZORDER));
            }
            return 0;
        }
        default:
            break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void Reset() noexcept {
        if (window_ != nullptr) {
            DestroyWindow(window_);
            window_ = nullptr;
        }
        if (class_reference_acquired_) {
            class_reference_acquired_ = false;
            if (g_fixture_window_class_references.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
                instance_ != nullptr) {
                static_cast<void>(UnregisterClassW(kClassName, instance_));
            }
        }
        instance_ = nullptr;
        minimized_ = false;
    }

    static constexpr const wchar_t* kClassName = L"AnomalyRenderFixtureWindow";
    HINSTANCE instance_{};
    HWND window_{};
    bool class_reference_acquired_{};
    bool minimized_{};
    UINT dpi_change_count_{};
    UINT last_dpi_{96};
};

struct Options final {
    UINT frames{3};
    UINT resizes{1};
    UINT rebuilds{1};
    UINT device_rebuilds{1};
    bool show_help{};
};

struct DeviceSelection final {
    ComPtr<ID3D12Device> device;
    bool warp{};
};

int Fail(const char* operation, HRESULT result) {
    std::cerr << operation << " failed (0x" << std::hex << std::setw(8)
              << std::setfill('0') << static_cast<std::uint32_t>(result) << ")\n";
    return 1;
}

anomaly::RuntimeStartContext FixtureRuntimeContext() {
    anomaly::RuntimeStartContext context;
    context.bootstrap_type = ANOMALY_BOOTSTRAP_TYPE_EXTERNAL;
    context.bootstrap_module = GetModuleHandleW(nullptr);
    context.game_module = context.bootstrap_module;
    context.runtime_root = L".";
    context.log_directory = L".";
    return context;
}

bool WaitForRuntimeState(
    const anomaly::RuntimeSession& session,
    AnomalyRuntimeState expected,
    std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (session.Snapshot().state == expected) return true;
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return session.Snapshot().state == expected;
}

bool ParseUint(std::string_view text, UINT& value) {
    if (text.empty()) return false;
    std::uint64_t parsed{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size() ||
        parsed > std::numeric_limits<UINT>::max()) {
        return false;
    }
    value = static_cast<UINT>(parsed);
    return true;
}

bool ParseOptions(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            options.show_help = true;
            return true;
        }

        UINT* destination{};
        std::string_view value;
        if (argument == "--frames") {
            destination = &options.frames;
        } else if (argument == "--resizes") {
            destination = &options.resizes;
        } else if (argument == "--rebuilds") {
            destination = &options.rebuilds;
        } else if (argument == "--device-rebuilds") {
            destination = &options.device_rebuilds;
        } else if (argument.starts_with("--frames=")) {
            destination = &options.frames;
            value = argument.substr(sizeof("--frames=") - 1);
        } else if (argument.starts_with("--resizes=")) {
            destination = &options.resizes;
            value = argument.substr(sizeof("--resizes=") - 1);
        } else if (argument.starts_with("--rebuilds=")) {
            destination = &options.rebuilds;
            value = argument.substr(sizeof("--rebuilds=") - 1);
        } else if (argument.starts_with("--device-rebuilds=")) {
            destination = &options.device_rebuilds;
            value = argument.substr(sizeof("--device-rebuilds=") - 1);
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            return false;
        }

        if (value.empty()) {
            if (++index >= argc) {
                std::cerr << argument << " requires a value\n";
                return false;
            }
            value = argv[index];
        }
        if (!ParseUint(value, *destination)) {
            std::cerr << argument << " has an invalid value\n";
            return false;
        }
    }

    if (options.resizes == 0 || options.rebuilds == 0 || options.device_rebuilds == 0 ||
        options.frames <= options.resizes || options.frames <= options.rebuilds ||
        options.frames <= options.device_rebuilds) {
        std::cerr << "frames must exceed nonzero resize, window rebuild, and device rebuild counts\n";
        return false;
    }
    return true;
}

bool IsHardwareAdapter(IDXGIAdapter1* adapter) {
    DXGI_ADAPTER_DESC1 description{};
    return SUCCEEDED(adapter->GetDesc1(&description)) &&
        (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0;
}

bool TryCreateDevice(IDXGIAdapter1* adapter, ComPtr<ID3D12Device>& device) {
    return SUCCEEDED(D3D12CreateDevice(
        adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.Put())));
}

HRESULT SelectDevice(IDXGIFactory4* factory, DeviceSelection& selection) {
    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(factory6.Put())))) {
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT result = factory6->EnumAdapterByGpuPreference(
                index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.Put()));
            if (result == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(result)) return result;
            if (IsHardwareAdapter(adapter.Get()) &&
                TryCreateDevice(adapter.Get(), selection.device)) {
                return S_OK;
            }
        }
    } else {
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT result = factory->EnumAdapters1(index, adapter.Put());
            if (result == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(result)) return result;
            if (IsHardwareAdapter(adapter.Get()) &&
                TryCreateDevice(adapter.Get(), selection.device)) {
                return S_OK;
            }
        }
    }

    ComPtr<IDXGIAdapter> warp_adapter;
    HRESULT result = factory->EnumWarpAdapter(IID_PPV_ARGS(warp_adapter.Put()));
    if (FAILED(result)) return result;
    result = D3D12CreateDevice(
        warp_adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(selection.device.Put()));
    if (SUCCEEDED(result)) selection.warp = true;
    return result;
}

HRESULT WaitForQueue(
    ID3D12CommandQueue* queue,
    ID3D12Fence* fence,
    HANDLE fence_event,
    UINT64& fence_value) {
    ++fence_value;
    HRESULT result = queue->Signal(fence, fence_value);
    if (FAILED(result)) return result;
    if (fence->GetCompletedValue() >= fence_value) return S_OK;

    result = fence->SetEventOnCompletion(fence_value, fence_event);
    if (FAILED(result)) return result;
    const DWORD wait = WaitForSingleObject(fence_event, 10'000);
    if (wait == WAIT_OBJECT_0) return S_OK;
    return wait == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT)
                                : HRESULT_FROM_WIN32(GetLastError());
}

HRESULT VerifySwapChain(
    IDXGISwapChain3* swap_chain, UINT width, UINT height, UINT buffer_count) {
    DXGI_SWAP_CHAIN_DESC1 swap_chain_description{};
    HRESULT result = swap_chain->GetDesc1(&swap_chain_description);
    if (FAILED(result)) return result;
    if (swap_chain_description.Width != width || swap_chain_description.Height != height ||
        swap_chain_description.BufferCount != buffer_count) {
        return E_UNEXPECTED;
    }

    if (swap_chain->GetCurrentBackBufferIndex() >= buffer_count) return E_UNEXPECTED;
    for (UINT index = 0; index < buffer_count; ++index) {
        ComPtr<ID3D12Resource> back_buffer;
        result = swap_chain->GetBuffer(index, IID_PPV_ARGS(back_buffer.Put()));
        if (FAILED(result) || !back_buffer) return FAILED(result) ? result : E_POINTER;
        const D3D12_RESOURCE_DESC resource_description = back_buffer->GetDesc();
        if (resource_description.Width != width || resource_description.Height != height ||
            resource_description.Format != kBackBufferFormat) {
            return E_UNEXPECTED;
        }
    }
    return S_OK;
}

HRESULT CreateFixtureSwapChain(
    IDXGIFactory4* factory,
    ID3D12CommandQueue* queue,
    HWND window,
    UINT width,
    UINT height,
    ComPtr<IDXGISwapChain3>& swap_chain) {
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width;
    description.Height = height;
    description.Format = kBackBufferFormat;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = kBufferCount;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> created;
    HRESULT result = factory->CreateSwapChainForHwnd(
        queue, window, &description, nullptr, nullptr, created.Put());
    if (FAILED(result)) return result;
    static_cast<void>(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER));
    return created->QueryInterface(IID_PPV_ARGS(swap_chain.Put()));
}

HRESULT PresentFixtureSwapChain(IDXGISwapChain3* swap_chain, bool use_present1) {
    if (swap_chain == nullptr) return E_POINTER;
    if (!use_present1) return swap_chain->Present(0, 0);
    DXGI_PRESENT_PARAMETERS parameters{};
    return swap_chain->Present1(0, 0, &parameters);
}

struct FixtureTexture final {
    ComPtr<ID3D12Resource> resource;
    std::array<std::uint8_t, 4> expected_pixel{};
};

HRESULT CreateFixtureTexture(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    ID3D12CommandAllocator* allocator,
    ID3D12GraphicsCommandList* command_list,
    ID3D12Fence* fence,
    HANDLE fence_event,
    UINT64& fence_value,
    const std::array<std::uint8_t, 4>& pixel,
    FixtureTexture& output) {
    if (device == nullptr || queue == nullptr || allocator == nullptr || command_list == nullptr ||
        fence == nullptr || fence_event == nullptr) {
        return E_POINTER;
    }

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = 2;
    texture.Height = 2;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = kBackBufferFormat;
    texture.SampleDesc.Count = 1;
    texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ComPtr<ID3D12Resource> resource;
    HRESULT result = device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(resource.Put()));
    if (FAILED(result)) return result;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total_bytes{};
    device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, nullptr, nullptr, &total_bytes);
    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upload{};
    upload.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload.Width = total_bytes;
    upload.Height = 1;
    upload.DepthOrArraySize = 1;
    upload.MipLevels = 1;
    upload.SampleDesc.Count = 1;
    upload.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload_resource;
    result = device->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE, &upload, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(upload_resource.Put()));
    if (FAILED(result)) return result;

    void* mapped{};
    const D3D12_RANGE no_read{};
    result = upload_resource->Map(0, &no_read, &mapped);
    if (FAILED(result) || mapped == nullptr) return FAILED(result) ? result : E_POINTER;
    auto* const bytes = static_cast<std::uint8_t*>(mapped) + footprint.Offset;
    for (UINT row = 0; row < texture.Height; ++row) {
        auto* const destination = bytes + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch;
        for (UINT column = 0; column < texture.Width; ++column) {
            std::copy(pixel.begin(), pixel.end(), destination + column * pixel.size());
        }
    }
    const D3D12_RANGE written_range{footprint.Offset, footprint.Offset + total_bytes};
    upload_resource->Unmap(0, &written_range);

    result = allocator->Reset();
    if (FAILED(result)) return result;
    result = command_list->Reset(allocator, nullptr);
    if (FAILED(result)) return result;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload_resource.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    D3D12_RESOURCE_BARRIER to_copy_source{};
    to_copy_source.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_copy_source.Transition.pResource = resource.Get();
    to_copy_source.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_copy_source.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    to_copy_source.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    command_list->ResourceBarrier(1, &to_copy_source);
    result = command_list->Close();
    if (FAILED(result)) return result;
    ID3D12CommandList* command_lists[]{command_list};
    queue->ExecuteCommandLists(1, command_lists);
    result = WaitForQueue(queue, fence, fence_event, fence_value);
    if (FAILED(result)) return result;

    output.resource = std::move(resource);
    output.expected_pixel = pixel;
    return S_OK;
}

HRESULT CaptureFixtureTexturePixel(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    ID3D12CommandAllocator* allocator,
    ID3D12GraphicsCommandList* command_list,
    ID3D12Fence* fence,
    HANDLE fence_event,
    UINT64& fence_value,
    ID3D12Resource* source_resource,
    ComPtr<ID3D12Resource>& readback,
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint) {
    if (device == nullptr || queue == nullptr || allocator == nullptr || command_list == nullptr ||
        fence == nullptr || fence_event == nullptr || source_resource == nullptr) {
        return E_POINTER;
    }

    const D3D12_RESOURCE_DESC source_description = source_resource->GetDesc();
    UINT64 total_bytes{};
    device->GetCopyableFootprints(
        &source_description, 0, 1, 0, &footprint, nullptr, nullptr, &total_bytes);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total_bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT result = device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(readback.Put()));
    if (FAILED(result)) return result;

    result = allocator->Reset();
    if (FAILED(result)) return result;
    result = command_list->Reset(allocator, nullptr);
    if (FAILED(result)) return result;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = source_resource;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    result = command_list->Close();
    if (FAILED(result)) return result;
    ID3D12CommandList* command_lists[]{command_list};
    queue->ExecuteCommandLists(1, command_lists);
    return WaitForQueue(queue, fence, fence_event, fence_value);
}

bool CapturedPixelMatches(
    ID3D12Resource* readback,
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
    const std::array<std::uint8_t, 4>& expected) {
    if (readback == nullptr) return false;
    const D3D12_RANGE read_range{footprint.Offset, footprint.Offset + expected.size()};
    void* mapped{};
    if (FAILED(readback->Map(0, &read_range, &mapped)) || mapped == nullptr) return false;
    const auto* const pixel = static_cast<const std::uint8_t*>(mapped) + footprint.Offset;
    const bool matches = std::equal(expected.begin(), expected.end(), pixel);
    const D3D12_RANGE written_range{};
    readback->Unmap(0, &written_range);
    return matches;
}
HRESULT RenderColorFrame(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    IDXGISwapChain3* swap_chain,
    ID3D12DescriptorHeap* render_target_heap,
    ID3D12CommandAllocator* allocator,
    ID3D12GraphicsCommandList* command_list,
    bool capture,
    ComPtr<ID3D12Resource>& readback,
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT& capture_footprint) {
    ComPtr<ID3D12Resource> back_buffer;
    HRESULT result = swap_chain->GetBuffer(
        swap_chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(back_buffer.Put()));
    if (FAILED(result)) return result;
    const auto render_target = render_target_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(back_buffer.Get(), nullptr, render_target);
    result = allocator->Reset();
    if (FAILED(result)) return result;
    result = command_list->Reset(allocator, nullptr);
    if (FAILED(result)) return result;

    D3D12_RESOURCE_BARRIER to_render_target{};
    to_render_target.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_render_target.Transition.pResource = back_buffer.Get();
    to_render_target.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_render_target.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    to_render_target.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    command_list->ResourceBarrier(1, &to_render_target);
    command_list->OMSetRenderTargets(1, &render_target, FALSE, nullptr);
    constexpr float clear_color[]{0.15F, 0.35F, 0.75F, 1.0F};
    command_list->ClearRenderTargetView(render_target, clear_color, 0, nullptr);

    D3D12_RESOURCE_BARRIER after = to_render_target;
    after.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    after.Transition.StateAfter = capture
        ? D3D12_RESOURCE_STATE_COPY_SOURCE
        : D3D12_RESOURCE_STATE_PRESENT;
    command_list->ResourceBarrier(1, &after);
    if (capture) {
        UINT64 total_bytes{};
        const D3D12_RESOURCE_DESC texture_description = back_buffer->GetDesc();
        device->GetCopyableFootprints(
            &texture_description, 0, 1, 0, &capture_footprint, nullptr, nullptr, &total_bytes);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = total_bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        result = device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(readback.Put()));
        if (FAILED(result)) return result;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = capture_footprint;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = back_buffer.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;
        command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        D3D12_RESOURCE_BARRIER to_present = after;
        to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        command_list->ResourceBarrier(1, &to_present);
    }
    result = command_list->Close();
    if (FAILED(result)) return result;
    ID3D12CommandList* command_lists[]{command_list};
    queue->ExecuteCommandLists(1, command_lists);
    return S_OK;
}

bool CapturedPixelIsNonEmpty(
    ID3D12Resource* readback,
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint) {
    if (readback == nullptr) return false;
    const D3D12_RANGE read_range{footprint.Offset, footprint.Offset + 4};
    void* mapped{};
    if (FAILED(readback->Map(0, &read_range, &mapped)) || mapped == nullptr) return false;
    const auto* pixel = static_cast<const std::uint8_t*>(mapped) + footprint.Offset;
    const bool non_empty = (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0) && pixel[3] != 0;
    const D3D12_RANGE written_range{};
    readback->Unmap(0, &written_range);
    return non_empty;
}

bool DebugQueueHasErrors(ID3D12InfoQueue* queue) {
    if (queue == nullptr) return false;
    const UINT64 count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 index = 0; index < count; ++index) {
        SIZE_T size{};
        if (FAILED(queue->GetMessage(index, nullptr, &size)) || size < sizeof(D3D12_MESSAGE)) {
            continue;
        }
        std::vector<std::uint8_t> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (SUCCEEDED(queue->GetMessage(index, message, &size)) &&
            (message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
             message->Severity == D3D12_MESSAGE_SEVERITY_ERROR)) {
            std::cerr << "D3D12 debug error: "
                      << (message->pDescription == nullptr ? "" : message->pDescription) << '\n';
            return true;
        }
    }
    return false;
}

void PumpWindowMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, options)) return 2;
    if (options.show_help) {
        std::cout << "usage: anomaly-render-fixture [--frames N] [--resizes N] [--rebuilds N] [--device-rebuilds N]\n";
        return 0;
    }

    FixtureWindow window;
    HRESULT result = window.Create(kInitialWidth, kInitialHeight);
    if (FAILED(result)) return Fail("CreateWindow", result);
    FixtureWindow companion_window;
    result = companion_window.Create(kInitialWidth / 2, kInitialHeight / 2);
    if (FAILED(result)) return Fail("Create companion window", result);

    ComPtr<ID3D12Debug> debug;
    const bool debug_layer = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.Put())));
    if (debug_layer) debug->EnableDebugLayer();

    ComPtr<IDXGIFactory4> factory;
    result = CreateDXGIFactory2(0, IID_PPV_ARGS(factory.Put()));
    if (FAILED(result)) return Fail("CreateDXGIFactory2", result);

    DeviceSelection selection;
    result = SelectDevice(factory.Get(), selection);
    if (FAILED(result)) return Fail("D3D12CreateDevice", result);
    ComPtr<ID3D12InfoQueue> debug_queue;
    if (debug_layer && SUCCEEDED(selection.device->QueryInterface(IID_PPV_ARGS(debug_queue.Put())))) {
        debug_queue->ClearStoredMessages();
    }

    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    result = selection.device->CreateCommandQueue(
        &queue_description, IID_PPV_ARGS(queue.Put()));
    if (FAILED(result)) return Fail("CreateCommandQueue", result);
    ComPtr<ID3D12CommandQueue> distractor_queue;
    result = selection.device->CreateCommandQueue(
        &queue_description, IID_PPV_ARGS(distractor_queue.Put()));
    if (FAILED(result)) return Fail("Create distractor command queue", result);

    ComPtr<IDXGISwapChain3> swap_chain;
    result = CreateFixtureSwapChain(
        factory.Get(), queue.Get(), window.Get(), kInitialWidth, kInitialHeight, swap_chain);
    if (FAILED(result)) return Fail("CreateFixtureSwapChain", result);
    ComPtr<IDXGISwapChain3> companion_swap_chain;
    result = CreateFixtureSwapChain(
        factory.Get(), queue.Get(), companion_window.Get(),
        kInitialWidth / 2, kInitialHeight / 2, companion_swap_chain);
    if (FAILED(result)) return Fail("Create companion swap chain", result);

    ComPtr<ID3D12Fence> fence;
    result = selection.device->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.Put()));
    if (FAILED(result)) return Fail("CreateFence", result);
    ComPtr<ID3D12Fence> distractor_fence;
    result = selection.device->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(distractor_fence.Put()));
    if (FAILED(result)) return Fail("Create distractor fence", result);

    UniqueHandle fence_event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!fence_event) return Fail("CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
    UniqueHandle distractor_fence_event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!distractor_fence_event) {
        return Fail("Create distractor fence event", HRESULT_FROM_WIN32(GetLastError()));
    }
    UINT64 fence_value{};
    UINT64 distractor_fence_value{};

    D3D12_DESCRIPTOR_HEAP_DESC render_target_heap_description{};
    render_target_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    render_target_heap_description.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> render_target_heap;
    result = selection.device->CreateDescriptorHeap(
        &render_target_heap_description, IID_PPV_ARGS(render_target_heap.Put()));
    if (FAILED(result)) return Fail("CreateDescriptorHeap", result);
    ComPtr<ID3D12CommandAllocator> command_allocator;
    result = selection.device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(command_allocator.Put()));
    if (FAILED(result)) return Fail("CreateCommandAllocator", result);
    ComPtr<ID3D12GraphicsCommandList> command_list;
    result = selection.device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocator.Get(), nullptr,
        IID_PPV_ARGS(command_list.Put()));
    if (FAILED(result)) return Fail("CreateCommandList", result);
    result = command_list->Close();
    if (FAILED(result)) return Fail("Close command list", result);
    FixtureTexture font_atlas;
    result = CreateFixtureTexture(
        selection.device.Get(), queue.Get(), command_allocator.Get(), command_list.Get(), fence.Get(),
        fence_event.Get(), fence_value, {0xEE, 0xE8, 0xD5, 0xFF}, font_atlas);
    if (FAILED(result)) return Fail("upload fixture font atlas", result);
    FixtureTexture texture;
    result = CreateFixtureTexture(
        selection.device.Get(), queue.Get(), command_allocator.Get(), command_list.Get(), fence.Get(),
        fence_event.Get(), fence_value, {0x20, 0xA0, 0x60, 0xFF}, texture);
    if (FAILED(result)) return Fail("upload fixture texture", result);
    ComPtr<ID3D12CommandAllocator> distractor_allocator;
    result = selection.device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(distractor_allocator.Put()));
    if (FAILED(result)) return Fail("Create distractor command allocator", result);
    ComPtr<ID3D12GraphicsCommandList> distractor_list;
    result = selection.device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, distractor_allocator.Get(), nullptr,
        IID_PPV_ARGS(distractor_list.Put()));
    if (FAILED(result)) return Fail("Create distractor command list", result);
    result = distractor_list->Close();
    if (FAILED(result)) return Fail("Close distractor command list", result);
    ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT capture_footprint{};
    ComPtr<ID3D12Resource> companion_readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT companion_capture_footprint{};
    ComPtr<ID3D12Resource> font_atlas_readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT font_atlas_footprint{};
    ComPtr<ID3D12Resource> texture_readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT texture_footprint{};

    anomaly::RuntimeSessionOptions runtime_options;
    runtime_options.dispatcher_options.worker_threads = 1;
    anomaly::RuntimeSession session(FixtureRuntimeContext(), std::move(runtime_options));
    const DWORD start_result = session.Start();
    if (start_result != ERROR_SUCCESS) {
        return Fail("RuntimeSession::Start", HRESULT_FROM_WIN32(start_result));
    }
    if (!WaitForRuntimeState(session, ANOMALY_RUNTIME_STATE_RUNNING)) {
        return Fail("wait for RuntimeSession Running", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }

    const auto runtime_snapshot = session.Snapshot();
    const std::uint64_t runtime_generation = runtime_snapshot.generation;
    const std::thread::id render_loop_thread = std::this_thread::get_id();
    std::vector<UINT> callback_order;
    callback_order.reserve(options.frames);
    std::vector<UINT> callbacks_by_resize_epoch(options.resizes + 1);
    std::vector<std::chrono::steady_clock::duration> render_dispatch_samples;
    render_dispatch_samples.reserve(options.frames);
    std::vector<std::chrono::steady_clock::duration> render_frame_cpu_samples;
    render_frame_cpu_samples.reserve(options.frames);
    bool callbacks_used_render_thread = true;

    UINT width = kInitialWidth;
    UINT height = kInitialHeight;
    UINT buffer_count = kBufferCount;
    UINT resize_count{};
    UINT rebuild_count{};
    UINT device_rebuild_count{};
    UINT primary_present1_count{};
    UINT companion_present1_count{};
    bool minimized_exercised{};
    bool dpi_exercised{};

    const auto release_device_objects = [&]() -> HRESULT {
        if (queue && fence) {
            const HRESULT wait_result =
                WaitForQueue(queue.Get(), fence.Get(), fence_event.Get(), fence_value);
            if (FAILED(wait_result)) return wait_result;
        }
        if (distractor_queue && distractor_fence) {
            const HRESULT wait_result = WaitForQueue(
                distractor_queue.Get(), distractor_fence.Get(), distractor_fence_event.Get(),
                distractor_fence_value);
            if (FAILED(wait_result)) return wait_result;
        }

        texture_readback.Reset();
        font_atlas_readback.Reset();
        companion_readback.Reset();
        readback.Reset();
        texture.resource.Reset();
        font_atlas.resource.Reset();
        distractor_list.Reset();
        distractor_allocator.Reset();
        command_list.Reset();
        command_allocator.Reset();
        render_target_heap.Reset();
        companion_swap_chain.Reset();
        swap_chain.Reset();
        distractor_fence.Reset();
        fence.Reset();
        distractor_queue.Reset();
        queue.Reset();

        if (debug_layer && selection.device) {
            ComPtr<ID3D12DebugDevice> debug_device;
            if (SUCCEEDED(selection.device->QueryInterface(IID_PPV_ARGS(debug_device.Put())))) {
                static_cast<void>(debug_device->ReportLiveDeviceObjects(
                    D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL));
            }
            if (DebugQueueHasErrors(debug_queue.Get())) return E_UNEXPECTED;
        }
        debug_queue.Reset();
        selection.device.Reset();
        return S_OK;
    };

    const auto create_device_objects = [&]() -> HRESULT {
        DeviceSelection refreshed_selection;
        HRESULT create_result = SelectDevice(factory.Get(), refreshed_selection);
        if (FAILED(create_result)) return create_result;
        selection = std::move(refreshed_selection);
        if (debug_layer &&
            SUCCEEDED(selection.device->QueryInterface(IID_PPV_ARGS(debug_queue.Put())))) {
            debug_queue->ClearStoredMessages();
        }

        D3D12_COMMAND_QUEUE_DESC queue_description{};
        queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        create_result = selection.device->CreateCommandQueue(
            &queue_description, IID_PPV_ARGS(queue.Put()));
        if (FAILED(create_result)) return create_result;
        create_result = selection.device->CreateCommandQueue(
            &queue_description, IID_PPV_ARGS(distractor_queue.Put()));
        if (FAILED(create_result)) return create_result;

        buffer_count = kBufferCount;
        create_result = CreateFixtureSwapChain(
            factory.Get(), queue.Get(), window.Get(), width, height, swap_chain);
        if (FAILED(create_result)) return create_result;
        create_result = CreateFixtureSwapChain(
            factory.Get(), queue.Get(), companion_window.Get(),
            kInitialWidth / 2, kInitialHeight / 2, companion_swap_chain);
        if (FAILED(create_result)) return create_result;

        create_result = selection.device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.Put()));
        if (FAILED(create_result)) return create_result;
        create_result = selection.device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(distractor_fence.Put()));
        if (FAILED(create_result)) return create_result;
        fence_value = 0;
        distractor_fence_value = 0;

        D3D12_DESCRIPTOR_HEAP_DESC render_target_heap_description{};
        render_target_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        render_target_heap_description.NumDescriptors = 1;
        create_result = selection.device->CreateDescriptorHeap(
            &render_target_heap_description, IID_PPV_ARGS(render_target_heap.Put()));
        if (FAILED(create_result)) return create_result;
        create_result = selection.device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(command_allocator.Put()));
        if (FAILED(create_result)) return create_result;
        create_result = selection.device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocator.Get(), nullptr,
            IID_PPV_ARGS(command_list.Put()));
        if (FAILED(create_result)) return create_result;
        create_result = command_list->Close();
        if (FAILED(create_result)) return create_result;

        create_result = CreateFixtureTexture(
            selection.device.Get(), queue.Get(), command_allocator.Get(), command_list.Get(),
            fence.Get(), fence_event.Get(), fence_value,
            std::array<std::uint8_t, 4>{0xEE, 0xE8, 0xD5, 0xFF}, font_atlas);
        if (FAILED(create_result)) return create_result;
        create_result = CreateFixtureTexture(
            selection.device.Get(), queue.Get(), command_allocator.Get(), command_list.Get(),
            fence.Get(), fence_event.Get(), fence_value,
            std::array<std::uint8_t, 4>{0x20, 0xA0, 0x60, 0xFF}, texture);
        if (FAILED(create_result)) return create_result;

        create_result = selection.device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(distractor_allocator.Put()));
        if (FAILED(create_result)) return create_result;
        create_result = selection.device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, distractor_allocator.Get(), nullptr,
            IID_PPV_ARGS(distractor_list.Put()));
        if (FAILED(create_result)) return create_result;
        return distractor_list->Close();
    };
    for (UINT frame = 0; frame < options.frames; ++frame) {
        const UINT device_rebuild_frame = static_cast<UINT>(
            (static_cast<UINT64>(device_rebuild_count + 1) * options.frames) /
            (options.device_rebuilds + 1));
        if (device_rebuild_count < options.device_rebuilds && frame == device_rebuild_frame) {
            result = release_device_objects();
            if (FAILED(result)) return Fail("release before controlled device rebuild", result);
            result = create_device_objects();
            if (FAILED(result)) return Fail("controlled device rebuild", result);
            ++device_rebuild_count;
        }

        if (!minimized_exercised && frame == options.frames / 2) {
            result = window.Minimize();
            if (FAILED(result)) return Fail("minimize fixture window", result);
            PumpWindowMessages();
            if (!window.IsMinimized()) return Fail("fixture window minimize state", E_UNEXPECTED);
            result = window.Restore();
            if (FAILED(result)) return Fail("restore fixture window", result);
            PumpWindowMessages();
            if (window.IsMinimized()) return Fail("fixture window restore state", E_UNEXPECTED);
            minimized_exercised = true;
        }

        if (!dpi_exercised && frame == options.frames / 2) {
            result = window.SimulateDpiChange(144);
            if (FAILED(result)) return Fail("primary DPI change", result);
            result = companion_window.SimulateDpiChange(120);
            if (FAILED(result)) return Fail("companion DPI change", result);
            PumpWindowMessages();
            if (window.LastDpi() != 144 || companion_window.LastDpi() != 120 ||
                window.DpiChangeCount() == 0 || companion_window.DpiChangeCount() == 0) {
                return Fail("DPI message delivery", E_UNEXPECTED);
            }
            dpi_exercised = true;
        }

        const UINT rebuild_frame =
            static_cast<UINT>((static_cast<UINT64>(rebuild_count + 1) * options.frames) /
                              (options.rebuilds + 1));
        if (rebuild_count < options.rebuilds && frame == rebuild_frame) {
            result = WaitForQueue(queue.Get(), fence.Get(), fence_event.Get(), fence_value);
            if (FAILED(result)) return Fail("wait before window rebuild", result);
            swap_chain.Reset();
            result = window.Recreate(width, height);
            if (FAILED(result)) return Fail("recreate window", result);
            result = CreateFixtureSwapChain(
                factory.Get(), queue.Get(), window.Get(), width, height, swap_chain);
            if (FAILED(result)) return Fail("recreate swap chain", result);
            buffer_count = kBufferCount;
            ++rebuild_count;
        }
        const UINT resize_frame =
            static_cast<UINT>((static_cast<UINT64>(resize_count + 1) * options.frames) /
                              (options.resizes + 1));
        if (resize_count < options.resizes && frame == resize_frame) {
            result = WaitForQueue(queue.Get(), fence.Get(), fence_event.Get(), fence_value);
            if (FAILED(result)) return Fail("wait before ResizeBuffers", result);

            width = resize_count % 2 == 0 ? kInitialWidth + 16 : kInitialWidth;
            height = resize_count % 2 == 0 ? kInitialHeight + 8 : kInitialHeight;
            buffer_count = resize_count % 2 == 0 ? kBufferCount + 1 : kBufferCount;
            result = window.Resize(width, height);
            if (FAILED(result)) return Fail("resize window", result);
            if (resize_count % 2 == 0) {
                result = swap_chain->ResizeBuffers(
                    buffer_count, width, height, kBackBufferFormat, 0);
            } else {
                const UINT creation_node_masks[kBufferCount + 1]{};
                IUnknown* present_queues[kBufferCount + 1]{
                    queue.Get(), queue.Get(), queue.Get()};
                result = swap_chain->ResizeBuffers1(
                    buffer_count, width, height, kBackBufferFormat, 0,
                    creation_node_masks, present_queues);
            }
            if (FAILED(result)) return Fail("ResizeBuffers", result);
            ++resize_count;
        }

        result = VerifySwapChain(swap_chain.Get(), width, height, buffer_count);
        if (FAILED(result)) return Fail("verify primary backbuffers", result);
        result = VerifySwapChain(
            companion_swap_chain.Get(), kInitialWidth / 2, kInitialHeight / 2, kBufferCount);
        if (FAILED(result)) return Fail("verify companion backbuffers", result);

        result = WaitForQueue(queue.Get(), fence.Get(), fence_event.Get(), fence_value);
        if (FAILED(result)) return Fail("wait before render", result);
        const auto frame_cpu_started = std::chrono::steady_clock::now();

        const UINT resize_epoch = resize_count;
        const auto render_task = session.Dispatchers().Post(
            anomaly::ExecutionDomain::Render,
            "render-fixture",
            runtime_generation,
            [&, frame, resize_epoch] {
                callbacks_used_render_thread = callbacks_used_render_thread &&
                    std::this_thread::get_id() == render_loop_thread;
                callback_order.push_back(frame);
                ++callbacks_by_resize_epoch[resize_epoch];
            });
        if (!render_task) return Fail("post Render task", E_UNEXPECTED);
        const auto dispatch_started = std::chrono::steady_clock::now();
        const std::size_t pumped =
            session.Dispatchers().PumpRender(kMaximumRenderCallbacksPerFrame);
        render_dispatch_samples.push_back(std::chrono::steady_clock::now() - dispatch_started);
        if (pumped != 1) {
            return Fail("pump Render task", E_UNEXPECTED);
        }
        const auto completed_task = session.Dispatchers().GetTask(render_task);
        if (!completed_task || completed_task->state != anomaly::TaskState::Completed) {
            return Fail("complete Render task", E_UNEXPECTED);
        }
        if (session.Dispatchers().BoundThread(anomaly::ExecutionDomain::Render) !=
            render_loop_thread) {
            return Fail("bind Render dispatcher", E_UNEXPECTED);
        }

        const bool capture_frame = frame + 1 == options.frames;
        result = RenderColorFrame(
            selection.device.Get(), queue.Get(), swap_chain.Get(), render_target_heap.Get(),
            command_allocator.Get(), command_list.Get(), capture_frame, readback, capture_footprint);
        if (FAILED(result)) return Fail("render primary color frame", result);
        const bool use_primary_present1 = (frame % 2) != 0;
        result = PresentFixtureSwapChain(swap_chain.Get(), use_primary_present1);
        if (FAILED(result)) return Fail("present primary swap chain", result);
        if (use_primary_present1) ++primary_present1_count;

        result = WaitForQueue(queue.Get(), fence.Get(), fence_event.Get(), fence_value);
        if (FAILED(result)) return Fail("wait between fixture windows", result);
        result = RenderColorFrame(
            selection.device.Get(), queue.Get(), companion_swap_chain.Get(), render_target_heap.Get(),
            command_allocator.Get(), command_list.Get(), capture_frame, companion_readback,
            companion_capture_footprint);
        if (FAILED(result)) return Fail("render companion color frame", result);
        result = PresentFixtureSwapChain(companion_swap_chain.Get(), true);
        if (FAILED(result)) return Fail("Present1 companion swap chain", result);
        ++companion_present1_count;

        if (resize_count != 0) {
            ID3D12CommandList* distractor_lists[]{distractor_list.Get()};
            distractor_queue->ExecuteCommandLists(1, distractor_lists);
            result = WaitForQueue(
                distractor_queue.Get(), distractor_fence.Get(),
                distractor_fence_event.Get(), distractor_fence_value);
            if (FAILED(result)) return Fail("wait for distractor queue", result);
        }
        render_frame_cpu_samples.push_back(
            std::chrono::steady_clock::now() - frame_cpu_started);
        PumpWindowMessages();
    }

    if (resize_count != options.resizes) return Fail("resize schedule", E_UNEXPECTED);
    if (rebuild_count != options.rebuilds) return Fail("window rebuild schedule", E_UNEXPECTED);
    if (device_rebuild_count != options.device_rebuilds) {
        return Fail("device rebuild schedule", E_UNEXPECTED);
    }
    if (!minimized_exercised || !dpi_exercised || primary_present1_count == 0 ||
        companion_present1_count != options.frames) {
        return Fail("window lifecycle matrix", E_UNEXPECTED);
    }
    result = WaitForQueue(queue.Get(), fence.Get(), fence_event.Get(), fence_value);
    if (FAILED(result)) return Fail("final GPU wait", result);
    if (!CapturedPixelIsNonEmpty(readback.Get(), capture_footprint) ||
        !CapturedPixelIsNonEmpty(companion_readback.Get(), companion_capture_footprint)) {
        return Fail("non-empty swap-chain pixel check", E_UNEXPECTED);
    }
    result = CaptureFixtureTexturePixel(
        selection.device.Get(), queue.Get(), command_allocator.Get(), command_list.Get(), fence.Get(),
        fence_event.Get(), fence_value, font_atlas.resource.Get(), font_atlas_readback,
        font_atlas_footprint);
    if (FAILED(result)) return Fail("capture fixture font atlas", result);
    if (!CapturedPixelMatches(
            font_atlas_readback.Get(), font_atlas_footprint, font_atlas.expected_pixel)) {
        return Fail("fixture font atlas pixel", E_UNEXPECTED);
    }
    result = CaptureFixtureTexturePixel(
        selection.device.Get(), queue.Get(), command_allocator.Get(), command_list.Get(), fence.Get(),
        fence_event.Get(), fence_value, texture.resource.Get(), texture_readback, texture_footprint);
    if (FAILED(result)) return Fail("capture fixture texture", result);
    if (!CapturedPixelMatches(texture_readback.Get(), texture_footprint, texture.expected_pixel)) {
        return Fail("fixture texture pixel", E_UNEXPECTED);
    }

    if (!callbacks_used_render_thread || callback_order.size() != options.frames) {
        return Fail("Render callback count/thread", E_UNEXPECTED);
    }
    for (UINT frame = 0; frame < options.frames; ++frame) {
        if (callback_order[frame] != frame) return Fail("Render callback order", E_UNEXPECTED);
    }
    for (UINT resize_epoch = 0; resize_epoch <= options.resizes; ++resize_epoch) {
        if (callbacks_by_resize_epoch[resize_epoch] == 0) {
            return Fail("Render callback resize continuity", E_UNEXPECTED);
        }
    }
    std::sort(render_dispatch_samples.begin(), render_dispatch_samples.end());
    const std::size_t render_p95_index =
        (render_dispatch_samples.size() * 95U + 99U) / 100U - 1U;
    const double render_dispatch_p95_ms =
        std::chrono::duration<double, std::milli>(
            render_dispatch_samples[render_p95_index]).count();
    if (render_dispatch_p95_ms > 0.30) {
        return Fail("Render dispatcher p95 budget", E_UNEXPECTED);
    }
    std::sort(render_frame_cpu_samples.begin(), render_frame_cpu_samples.end());
    const std::size_t render_frame_p95_index =
        (render_frame_cpu_samples.size() * 95U + 99U) / 100U - 1U;
    const double render_frame_cpu_p95_ms =
        std::chrono::duration<double, std::milli>(
            render_frame_cpu_samples[render_frame_p95_index]).count();
    if (render_frame_cpu_p95_ms > 5.0) {
        return Fail("Render frame CPU p95 budget", E_UNEXPECTED);
    }

    UINT stopped_callback_count{};
    const auto pending_at_stop = session.Dispatchers().Post(
        anomaly::ExecutionDomain::Render,
        "render-fixture-stop",
        runtime_generation,
        [&] { ++stopped_callback_count; });
    if (!pending_at_stop) return Fail("post pending Render task", E_UNEXPECTED);

    session.RequestStop();
    if (!session.WaitForStop(2s)) {
        return Fail("wait for RuntimeSession stop", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }
    session.Join();
    const auto stopped_snapshot = session.Snapshot();
    const auto stopped_task = session.Dispatchers().GetTask(pending_at_stop);
    if (stopped_snapshot.state != ANOMALY_RUNTIME_STATE_STOPPED ||
        stopped_snapshot.last_error != ERROR_SUCCESS || stopped_callback_count != 0 ||
        !stopped_task || stopped_task->state != anomaly::TaskState::Cancelled ||
        session.Dispatchers().IsAccepting() ||
        session.Dispatchers().PumpRender(kMaximumRenderCallbacksPerFrame) != 0) {
        return Fail("RuntimeSession dispatcher cleanup", E_UNEXPECTED);
    }
    if (session.Dispatchers().Post(
            anomaly::ExecutionDomain::Render,
            "render-fixture-stopped",
            runtime_generation,
            [] {})) {
        return Fail("post after RuntimeSession stop", E_UNEXPECTED);
    }

    result = release_device_objects();
    if (FAILED(result)) return Fail("D3D12 device cleanup", result);
    factory.Reset();
    debug.Reset();

    std::cout << "ok backend=" << (selection.warp ? "warp" : "hardware")
              << " frames=" << options.frames << " windows=2"
              << " resizes=" << resize_count << " window_rebuilds=" << rebuild_count
              << " device_rebuilds=" << device_rebuild_count
              << " present1_primary=" << primary_present1_count
              << " present1_companion=" << companion_present1_count
              << " minimized=1 dpi_changes=" << (window.DpiChangeCount() + companion_window.DpiChangeCount())
              << " font_texture_pixels=2 swap_chain_pixels=2"
              << " debug_layer=" << (debug_layer ? 1 : 0)
              << " render_callbacks=" << callback_order.size()
              << " render_dispatch_p95_ms=" << std::fixed << std::setprecision(6)
              << render_dispatch_p95_ms
              << " render_frame_cpu_p95_ms=" << render_frame_cpu_p95_ms
              << " size=" << width << 'x' << height << '\n';
    return 0;
}
