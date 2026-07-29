#include "embedded_host_internal.hpp"

namespace ue5mem::embedded {
namespace {

LRESULT CALLBACK DummyWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

HookTargets DiscoverD3D12HookTargets() {
    HookTargets targets;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* class_name = L"AnomalyD3D12HookProbe";
    WNDCLASSEXW window_class{
        sizeof(WNDCLASSEXW), CS_CLASSDC, DummyWindowProc, 0, 0, instance, nullptr, nullptr,
        nullptr, nullptr, class_name, nullptr};
    const ATOM atom = RegisterClassExW(&window_class);
    HWND window = CreateWindowExW(
        0, class_name, L"", WS_OVERLAPPEDWINDOW,
        0, 0, 100, 100, nullptr, nullptr, instance, nullptr);
    ID3D12Device* device{};
    ID3D12CommandQueue* queue{};
    IDXGIFactory4* factory{};
    IDXGISwapChain1* swap_chain{};
    IDXGISwapChain3* swap_chain3{};
    bool ready = window != nullptr &&
        SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    if (ready) {
        D3D12_COMMAND_QUEUE_DESC queue_description{};
        queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ready = SUCCEEDED(device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&queue))) &&
            SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    }
    if (ready) {
        DXGI_SWAP_CHAIN_DESC1 swap_description{};
        swap_description.Width = 100;
        swap_description.Height = 100;
        swap_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swap_description.SampleDesc.Count = 1;
        swap_description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap_description.BufferCount = 2;
        swap_description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ready = SUCCEEDED(factory->CreateSwapChainForHwnd(
            queue, window, &swap_description, nullptr, nullptr, &swap_chain));
    }
    if (ready) {
        ready = SUCCEEDED(swap_chain->QueryInterface(IID_PPV_ARGS(&swap_chain3)));
    }
    if (ready) {
        auto** queue_table = *reinterpret_cast<void***>(queue);
        auto** swap_table = *reinterpret_cast<void***>(swap_chain);
        auto** swap3_table = *reinterpret_cast<void***>(swap_chain3);
        targets.execute_command_lists = queue_table[10];
        targets.present = swap_table[8];
        targets.resize_buffers = swap_table[13];
        targets.present1 = swap_table[22];
        targets.resize_buffers1 = swap3_table[39];
    }
    Release(swap_chain3);
    Release(swap_chain);
    Release(factory);
    Release(queue);
    Release(device);
    if (window != nullptr) DestroyWindow(window);
    if (atom != 0) UnregisterClassW(class_name, instance);
    return targets;
}

}  // namespace ue5mem::embedded
