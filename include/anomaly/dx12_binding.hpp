#pragma once

#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <mutex>

namespace anomaly {

// Renderer publishes only a creation/ResizeBuffers1-proven pair. PluginManager
// owns this mailbox so Lifecycle/Worker never touch renderer-owned state.
class Dx12Binding final {
public:
    void Publish(IDXGISwapChain* swap_chain, ID3D12CommandQueue* queue) {
        std::scoped_lock lock(mutex_);
        if (stopped_) return;
        swap_chain_ = swap_chain;
        queue_ = queue;
    }
    void Withdraw() {
        std::scoped_lock lock(mutex_);
        swap_chain_.Reset();
        queue_.Reset();
    }
    void Stop() {
        std::scoped_lock lock(mutex_);
        stopped_ = true;
        swap_chain_.Reset();
        queue_.Reset();
    }
    [[nodiscard]] bool Acquire(void** swap_chain, void** queue) {
        std::scoped_lock lock(mutex_);
        if (stopped_ || !swap_chain_ || !queue_) return false;
        swap_chain_->AddRef();
        queue_->AddRef();
        *swap_chain = swap_chain_.Get();
        *queue = queue_.Get();
        return true;
    }

private:
    std::mutex mutex_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    bool stopped_{};
};

} // namespace anomaly
