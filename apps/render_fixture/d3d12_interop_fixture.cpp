#include "render/dx12/embedded_d3d12_interop.hpp"
#include "render/dx12/embedded_present_scope.hpp"

#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace ue5mem::embedded;

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Check(HRESULT result, const char* message) {
    Require(SUCCEEDED(result), message);
}

// Models the published Streamline IUnknown contract. It deliberately returns
// its own identity for ordinary QI, as SL proxies do, and exposes the native
// object only through StreamlineRetrieveBaseInterface. No proxy layout is used.
struct __declspec(uuid("ADEC44E2-61F0-45C3-AD9F-1B37379284FF")) PublishedStreamlineInterface;
class NativeProxy final : public IUnknown {
public:
    explicit NativeProxy(IUnknown* native) : native_(native) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
        if (result == nullptr) return E_POINTER;
        *result = nullptr;
        if (id == __uuidof(PublishedStreamlineInterface)) {
            return native_->QueryInterface(IID_IUnknown, result);
        }
        if (id == IID_IUnknown) {
            *result = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { return --references_; }
    ULONG References() const { return references_; }
private:
    ComPtr<IUnknown> native_;
    std::atomic<ULONG> references_{1};
};

bool SameObject(IUnknown* left, IUnknown* right) {
    ComPtr<IUnknown> a, b;
    return left != nullptr && right != nullptr &&
        SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&a))) &&
        SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&b))) && a == b;
}

void TestPresentationScopes() {
    anomaly::ThreadLocalScalar<bool> active;
    unsigned drawings{};
    unsigned forwards{};
    const auto native_present = [&] {
        EmbeddedPresentScope scope(active);
        if (scope.Outermost()) ++drawings;
        ++forwards;
        return E_ABORT;
    };
    const auto present1 = [&] {
        EmbeddedPresentScope scope(active);
        if (scope.Outermost()) ++drawings;
        ++forwards;
        return native_present();
    };
    const auto proxy_present = [&] {
        EmbeddedPresentScope scope(active);
        if (scope.Outermost()) ++drawings;
        ++forwards;
        return present1();
    };
    Require(proxy_present() == E_ABORT, "nested Present must preserve failure HRESULT");
    Require(drawings == 1 && forwards == 3, "one draw per outer Present, every call forwarded");
    Require(!active.Get(), "Present scope leaked after return");
    {
        EmbeddedPresentScope outer(active);
        bool other_thread_outer{};
        std::jthread other([&] {
            EmbeddedPresentScope independent(active);
            other_thread_outer = independent.Outermost();
        });
        other.join();
        Require(other_thread_outer, "independent present threads must not suppress each other");
    }
    try {
        EmbeddedPresentScope outer(active);
        throw std::runtime_error("unwind");
    } catch (const std::runtime_error&) {}
    Require(!active.Get(), "Present scope leaked after C++ unwind");
    std::puts("PASS: nested Present/Present1 forwarding, HRESULT, unwind, thread isolation");
}

void TestNativeInterop() {
    ComPtr<IDXGIFactory4> factory;
    Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "create DXGI factory");
    ComPtr<IDXGIAdapter> warp;
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "get WARP adapter");
    ComPtr<ID3D12Device> device;
    Check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(&device)), "create real D3D12 device");
    NativeProxy device_proxy(device.Get());
    ComPtr<ID3D12Device> normalized;
    normalized.Attach(AcquireNativeD3D12Interface<ID3D12Device>(&device_proxy));
    Require(SameObject(normalized.Get(), device.Get()), "proxy must resolve to native device");
    Require(device_proxy.References() == 1, "proxy reference leaked");
    Require(AcquireNativeD3D12Interface<ID3D12CommandQueue>(&device_proxy) == nullptr,
        "unsupported native interface must not be coerced to a queue");
    ComPtr<ID3D12Device> already_native;
    already_native.Attach(AcquireNativeD3D12Interface<ID3D12Device>(device.Get()));
    Require(SameObject(already_native.Get(), device.Get()), "native interface must remain native");
    Require(AcquireNativeD3D12Interface<ID3D12Device>(nullptr) == nullptr,
        "null source must remain unavailable");

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue_a, queue_b;
    Check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_a)), "create queue A");
    Check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_b)), "create queue B");
    NativeProxy queue_proxy(queue_a.Get());
    EmbeddedQueueCapture captured;
    captured.Observe(101, queue_a.Get());
    captured.Observe(202, queue_b.Get()); // Later DLSS worker submission.
    ComPtr<ID3D12CommandQueue> game_thread_queue;
    game_thread_queue.Attach(captured.Acquire(101));
    Require(SameObject(game_thread_queue.Get(), queue_a.Get()),
        "frame-generation worker replaced the application's queue");
    captured.Observe(101, queue_b.Get());
    game_thread_queue.Reset();
    game_thread_queue.Attach(captured.Acquire(101));
    Require(SameObject(game_thread_queue.Get(), queue_b.Get()),
        "application's own queue change was ignored");
    captured.Clear();
    Require(captured.Acquire(101) == nullptr, "captured queues survived teardown");

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"AnomalyD3D12InteropFixture";
    Require(RegisterClassW(&window_class) != 0, "register fixture window class");
    const auto create_window = [&] {
        return CreateWindowW(window_class.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
            0, 0, 64, 64, nullptr, nullptr, instance, nullptr);
    };
    const HWND window_a = create_window();
    const HWND window_b = create_window();
    Require(window_a != nullptr && window_b != nullptr, "create fixture windows");
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = desc.Height = 64;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain_a, chain_b;
    Check(factory->CreateSwapChainForHwnd(queue_a.Get(), window_a, &desc,
        nullptr, nullptr, &chain_a), "create swap chain A");
    Check(factory->CreateSwapChainForHwnd(queue_b.Get(), window_b, &desc,
        nullptr, nullptr, &chain_b), "create swap chain B");
    Require(AcquirePresentationQueue(chain_a.Get()) == nullptr,
        "unobserved swap chain must not invent a queue");
    BindPresentationQueue(chain_a.Get(), &queue_proxy);
    BindPresentationQueue(chain_b.Get(), queue_b.Get());
    ComPtr<ID3D12CommandQueue> bound_a, bound_b;
    bound_a.Attach(AcquirePresentationQueue(chain_a.Get()));
    bound_b.Attach(AcquirePresentationQueue(chain_b.Get()));
    Require(SameObject(bound_a.Get(), queue_a.Get()), "chain A lost its creation queue");
    Require(SameObject(bound_b.Get(), queue_b.Get()), "chain B captured another chain's queue");
    Require(!SameObject(bound_a.Get(), bound_b.Get()), "different direct queues were conflated");
    Require(queue_proxy.References() == 1, "swap chain retained proxy instead of native queue");
    bound_a.Reset();
    BindPresentationQueue(chain_a.Get(), queue_b.Get());
    bound_a.Attach(AcquirePresentationQueue(chain_a.Get()));
    Require(SameObject(bound_a.Get(), queue_b.Get()), "resize queue handoff was not adopted");

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    Check(normalized->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&allocator)), "create native allocator from proxy device");
    Check(normalized->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "create native list from proxy device");
    Check(list->Close(), "close native list");
    ID3D12CommandList* lists[]{list.Get()};
    bound_a->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    Check(normalized->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "create fence");
    Check(bound_a->Signal(fence.Get(), 1), "signal native queue after native submission");
    const HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(done != nullptr, "create fence event");
    Check(fence->SetEventOnCompletion(1, done), "arm fence event");
    const DWORD waited = WaitForSingleObject(done, 10000);
    CloseHandle(done);
    Require(waited == WAIT_OBJECT_0, "native GPU submission did not complete");
    Check(device->GetDeviceRemovedReason(), "native submission removed D3D12 device");
    chain_a.Reset();
    chain_b.Reset();
    DestroyWindow(window_a);
    DestroyWindow(window_b);
    UnregisterClassW(window_class.lpszClassName, instance);
    std::puts("PASS: real D3D12 native/proxy interfaces, per-thread queues, two swap-chain queues, queue handoff, GPU fence");
}

} // namespace

int main() {
    try {
        TestPresentationScopes();
        TestNativeInterop();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
