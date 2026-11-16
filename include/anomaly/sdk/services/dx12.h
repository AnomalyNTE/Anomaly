#pragma once
#include "anomaly/sdk/base.h"

#define ANOMALY_DX12_BINDING_SERVICE_V1_ID "anomaly.dx12.binding"
#define ANOMALY_DX12_BINDING_SERVICE_V1_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AnomalyDx12BindingServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    // Lifecycle/Worker only; requires the dx12-binding capability.
    // On OK, returns IDXGISwapChain* and its captured DIRECT ID3D12CommandQueue*,
    // each with one owned COM reference. Release both, including on consumer failure.
    // This is a point-in-time binding, not a render callback or device-lifetime lease.
    // Consumers own their rendering, resize/device-loss handling and stop/drain logic;
    // do not retain back buffers across ResizeBuffers. No host ImGui state is shared.
    // UNAVAILABLE before an exact binding is known, during resize/device loss/stop,
    // or for D3D11. Never substitutes a recently observed submission queue.
    // Both outputs are set to null on failure. Outputs must be distinct addresses.
    AnomalyStatusV1 (ANOMALY_CALL *acquire)(void* user, void** swap_chain, void** command_queue);
} AnomalyDx12BindingServiceV1;

#ifdef __cplusplus
}
#endif
