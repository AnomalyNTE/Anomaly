# Inter-plugin IPC

Corresponding header: `services/ipc.h`. This is the **only** mechanism for communication between plugins; it is not the [diagnostics CLI protocol](../user-guide/diagnostics-cli.md). All endpoints, subscriptions and pending call resources are bound to the plugin ID and generation. Payloads are bounded bytes owned by the caller. For general conventions see the [API reference overview](README.md).

- **ID**: `"anomaly.ipc"` · **version** 1 · **capability** `ipc`
- **schema hash size**: `ANOMALY_IPC_SCHEMA_HASH_V1_SIZE` = 32 bytes

## Enumerations

```c
typedef enum AnomalyIpcModeV1 {                  // bit flags
    ANOMALY_IPC_MODE_V1_SYNC_REQUEST  = 1u << 0u,
    ANOMALY_IPC_MODE_V1_ASYNC_REQUEST = 1u << 1u,
    ANOMALY_IPC_MODE_V1_EVENT         = 1u << 2u
} AnomalyIpcModeV1;

typedef enum AnomalyIpcAffinityV1 {
    ANOMALY_IPC_AFFINITY_V1_CALLER = 0, ANOMALY_IPC_AFFINITY_V1_WORKER = 1,
    ANOMALY_IPC_AFFINITY_V1_LIFECYCLE = 2, ANOMALY_IPC_AFFINITY_V1_GAME = 3,
    ANOMALY_IPC_AFFINITY_V1_RENDER = 4
} AnomalyIpcAffinityV1;

typedef enum AnomalyIpcReentrancyV1 {
    ANOMALY_IPC_REENTRANCY_V1_REJECT = 0, ANOMALY_IPC_REENTRANCY_V1_ALLOW = 1
} AnomalyIpcReentrancyV1;

typedef enum AnomalyIpcErrorV1 {
    ANOMALY_IPC_ERROR_V1_NONE = 0, ANOMALY_IPC_ERROR_V1_PROVIDER_MISSING = 1,
    ANOMALY_IPC_ERROR_V1_VERSION_MISMATCH = 2, ANOMALY_IPC_ERROR_V1_SCHEMA_MISMATCH = 3,
    ANOMALY_IPC_ERROR_V1_MODE_UNAVAILABLE = 4, ANOMALY_IPC_ERROR_V1_TIMEOUT = 5,
    ANOMALY_IPC_ERROR_V1_REENTRANT_CYCLE = 6, ANOMALY_IPC_ERROR_V1_QUEUE_FULL = 7,
    ANOMALY_IPC_ERROR_V1_STALE_GENERATION = 8, ANOMALY_IPC_ERROR_V1_DEPENDENCY_REQUIRED = 9
} AnomalyIpcErrorV1;
```

## Descriptor and selector structs

```c
typedef struct AnomalyIpcSchemaHashV1 { uint8_t bytes[ANOMALY_IPC_SCHEMA_HASH_V1_SIZE]; } AnomalyIpcSchemaHashV1;

typedef struct AnomalyIpcEndpointDescriptorV1 {
    uint32_t struct_size;
    AnomalyStringViewV1 endpoint_id;
    uint32_t major_version, minor_version;
    AnomalyIpcSchemaHashV1 request_schema, response_schema, event_schema;
    uint32_t modes;         // combination of AnomalyIpcModeV1 bits
    uint32_t affinity;      // AnomalyIpcAffinityV1
    uint32_t timeout_milliseconds;
    uint32_t reentrancy;    // AnomalyIpcReentrancyV1
    uint32_t maximum_request_bytes, maximum_response_bytes, maximum_event_bytes;
    uint32_t maximum_queue_depth;
} AnomalyIpcEndpointDescriptorV1;

typedef struct AnomalyIpcEndpointSelectorV1 {
    uint32_t struct_size;
    AnomalyStringViewV1 endpoint_id;
    uint32_t major_version, minimum_minor_version;
    AnomalyIpcSchemaHashV1 request_schema, response_schema, event_schema;
} AnomalyIpcEndpointSelectorV1;

typedef struct AnomalyIpcRequestContextV1 {
    uint32_t struct_size; uint32_t reserved;
    uint64_t request_id;
    AnomalyStringViewV1 caller_plugin_id;
} AnomalyIpcRequestContextV1;
```

The caller uses the selector to declare the expected `major_version`, `minimum_minor_version` and the three schema hashes; the host matches a provider endpoint against them and returns the corresponding error when the version or schema does not fit.

## Callback types

```c
typedef AnomalyStatusV1 (ANOMALY_CALL *AnomalyIpcRequestHandlerV1)(void* user,
    const AnomalyIpcRequestContextV1* context, AnomalyByteSpanV1 request,
    AnomalyMutableByteSpanV1 response, size_t* response_size);
typedef void (ANOMALY_CALL *AnomalyIpcCompletionCallbackV1)(void* user,
    AnomalyGenerationHandleV1 pending_call, AnomalyStatusV1 status, AnomalyByteSpanV1 response);
typedef void (ANOMALY_CALL *AnomalyIpcEventCallbackV1)(void* user,
    AnomalyStringViewV1 endpoint_id, AnomalyByteSpanV1 event);
```

## Service table

```c
typedef struct AnomalyIpcServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *register_endpoint)(void* user,
        const AnomalyIpcEndpointDescriptorV1* descriptor,
        AnomalyIpcRequestHandlerV1 request_handler, void* callback_user,
        AnomalyGenerationHandleV1* endpoint);
    AnomalyStatusV1 (ANOMALY_CALL *unregister_endpoint)(void* user, AnomalyGenerationHandleV1 endpoint);
    AnomalyStatusV1 (ANOMALY_CALL *invoke)(void* user,
        const AnomalyIpcEndpointSelectorV1* selector, AnomalyByteSpanV1 request,
        AnomalyMutableByteSpanV1 response, size_t* response_size);
    AnomalyStatusV1 (ANOMALY_CALL *invoke_async)(void* user,
        const AnomalyIpcEndpointSelectorV1* selector, AnomalyByteSpanV1 request,
        AnomalyIpcCompletionCallbackV1 completion, void* completion_user,
        AnomalyGenerationHandleV1* pending_call);
    AnomalyStatusV1 (ANOMALY_CALL *cancel)(void* user, AnomalyGenerationHandleV1 pending_call);
    AnomalyStatusV1 (ANOMALY_CALL *subscribe)(void* user,
        const AnomalyIpcEndpointSelectorV1* selector,
        AnomalyIpcEventCallbackV1 callback, void* callback_user,
        AnomalyGenerationHandleV1* subscription);
    AnomalyStatusV1 (ANOMALY_CALL *unsubscribe)(void* user, AnomalyGenerationHandleV1 subscription);
    AnomalyStatusV1 (ANOMALY_CALL *publish)(void* user,
        AnomalyGenerationHandleV1 endpoint, AnomalyByteSpanV1 event);
} AnomalyIpcServiceV1;
```

| Function | Description |
| --- | --- |
| `register_endpoint` | Registers a provider endpoint with its request handler; returns an endpoint handle |
| `unregister_endpoint` | Unregisters the endpoint |
| `invoke` | Synchronous request; `response` uses the two-phase buffer protocol |
| `invoke_async` | Asynchronous request; calls back `completion` when it completes; returns a pending call handle |
| `cancel` | Cancels a pending asynchronous call |
| `subscribe` / `unsubscribe` | Subscribes to / unsubscribes from endpoint events |
| `publish` | The provider endpoint publishes an event |

> [!NOTE]
> Dependencies: if the provider required by the selector belongs to a dependency you did not declare, the host may return `ANOMALY_IPC_ERROR_V1_DEPENDENCY_REQUIRED`. All endpoints, subscriptions and pending call resources are bound to the plugin ID and generation; for the ownership model see [Architecture overview · Ownership and lifecycle](../developer-guide/architecture.md#ownership-and-lifecycle).
