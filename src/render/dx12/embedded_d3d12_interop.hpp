#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <unordered_map>

namespace ue5mem::embedded {

// NVIDIA Streamline's QueryInterface contract, not a proxy object layout.
// See source/core/sl.api/internal.h and sl.interposer/*/QueryInterface.
inline constexpr GUID kStreamlineNativeInterface{
    0xadec44e2, 0x61f0, 0x45c3, {0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff}};
inline constexpr GUID kEmbeddedPresentationQueue{
    0xd6906f7b, 0x24a6, 0x44c9, {0x82, 0x35, 0xd2, 0xc4, 0xf1, 0x98, 0x5d, 0x13}};

// Returns an owned reference. Native queues must receive native command lists;
// creating the latter with a proxy device would mix the two interface families.
// Do not apply this to swap chains: DLSS owns their buffers and buffer indices.
template <typename T>
[[nodiscard]] T* AcquireNativeD3D12Interface(IUnknown* source) noexcept {
    if (source == nullptr) return nullptr;
    IUnknown* native{};
    const HRESULT unwrapped = source->QueryInterface(
        kStreamlineNativeInterface, reinterpret_cast<void**>(&native));
    T* result{};
    if (SUCCEEDED(unwrapped) && native != nullptr) {
        static_cast<void>(native->QueryInterface(IID_PPV_ARGS(&result)));
        native->Release();
    } else {
        static_cast<void>(source->QueryInterface(IID_PPV_ARGS(&result)));
    }
    return result;
}

inline void BindPresentationQueue(IDXGISwapChain* swap_chain, IUnknown* device) noexcept {
    if (swap_chain == nullptr) return;
    auto* queue = AcquireNativeD3D12Interface<ID3D12CommandQueue>(device);
    if (queue == nullptr) return; // DXGI also accepts D3D11 devices here.
    if (queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        // DXGI owns this reference until the chain is destroyed/rebound.
        static_cast<void>(swap_chain->SetPrivateDataInterface(kEmbeddedPresentationQueue, queue));
    }
    queue->Release();
}

[[nodiscard]] inline ID3D12CommandQueue* AcquirePresentationQueue(
    IDXGISwapChain* swap_chain) noexcept {
    ID3D12CommandQueue* queue{};
    UINT size = sizeof(queue);
    if (swap_chain == nullptr || FAILED(swap_chain->GetPrivateData(
            kEmbeddedPresentationQueue, &size, &queue))) return nullptr;
    return queue; // GetPrivateData AddRefs interfaces stored by SetPrivateDataInterface.
}

// Used for late attachment, when swap-chain creation has already happened.
// Frame generation submits on its own threads/queues; their last submission
// must not replace the queue observed on the application's Present thread.
// The caller holds EmbeddedState::queue_mutex for every operation.
class EmbeddedQueueCapture final {
public:
    void Observe(DWORD thread, ID3D12CommandQueue* queue) {
        auto& observed = threads_[thread];
        if (observed.Get() != queue) observed = queue;
        if (latest_.Get() != queue) latest_ = queue;
    }
    [[nodiscard]] ID3D12CommandQueue* Acquire(DWORD thread) const noexcept {
        const auto found = threads_.find(thread);
        return AcquireNativeD3D12Interface<ID3D12CommandQueue>(
            found != threads_.end() ? found->second.Get() : latest_.Get());
    }
    void Clear() noexcept {
        threads_.clear();
        latest_.Reset();
    }

private:
    std::unordered_map<DWORD, Microsoft::WRL::ComPtr<ID3D12CommandQueue>> threads_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> latest_;
};

} // namespace ue5mem::embedded
