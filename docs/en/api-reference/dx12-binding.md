# DX12 binding

Header: `anomaly/sdk/services/dx12.h`. Service `anomaly.dx12.binding` v1; capability `dx12-binding`.

Allows a late-loaded plugin with its own DX12 renderer to acquire the current swap chain
and its matching DIRECT command queue. It shares no host ImGui context and does not install
hooks, render, or manage input for the consumer.

```json
"capabilities": ["dx12-binding"],
"services": [{"id": "anomaly.dx12.binding", "minVersion": 1, "optional": true}]
```

Query `AnomalyDx12BindingServiceV1`, validate `struct_size`, `service_version` and `acquire`,
then call `acquire(user, &swap, &queue)` on Lifecycle / Worker only. Render, Game and
unregistered threads receive `UNAVAILABLE`; unauthorized queries receive `PERMISSION_DENIED`.

- `OK`: outputs are `IDXGISwapChain*` and `ID3D12CommandQueue*`, each with one owned COM
  reference. Release both, including when consumer initialization fails. There is no host
  callback or handle to return.
- `UNAVAILABLE`: no creation/successful-ResizeBuffers1-proven pair, D3D11, resize in progress,
  device loss or stop. The renderer's last-submitted-queue fallback is never exported.
- Null or aliased output addresses return `INVALID_ARGUMENT`; valid outputs are cleared on failure.

The service table belongs to the plugin generation and must not be used after unload.
COM references keep objects alive, but are not device-validity, window or render-epoch leases.
Consumers must handle Present, resize and device loss, and must not retain back buffers across
`ResizeBuffers`. Stop and drain the independent renderer before releasing references on plugin
stop. Withdrawal prevents new acquisitions without revoking already-owned references; reacquire
after a device or swap-chain change.

Declare an optional dependency to support older hosts and report missing service/readiness.
A queryable table does not imply graphics readiness; never wait for initialization on Render.
If the framework itself attaches after swap-chain creation and observes no later valid
`ResizeBuffers1`, acquisition can remain `UNAVAILABLE` rather than guessing the pair.
