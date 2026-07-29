# 插件间 IPC

对应头文件：`services/ipc.h`。这是插件之间通信的**唯一**机制；它不是[诊断管道协议](../user-guide/diagnostics-cli.md)。所有端点、订阅与 pending call 资源绑定到插件 ID 与 generation。载荷是有界、调用方所有的字节。通用约定见 [API 参考总览](README.md)。

- **ID**：`"anomaly.ipc"` · **版本** 1 · **capability** `ipc`
- **schema hash 大小**：`ANOMALY_IPC_SCHEMA_HASH_V1_SIZE` = 32 字节

## 枚举

```c
typedef enum AnomalyIpcModeV1 {                  // 位标志
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

## 描述与选择结构

```c
typedef struct AnomalyIpcSchemaHashV1 { uint8_t bytes[ANOMALY_IPC_SCHEMA_HASH_V1_SIZE]; } AnomalyIpcSchemaHashV1;

typedef struct AnomalyIpcEndpointDescriptorV1 {
    uint32_t struct_size;
    AnomalyStringViewV1 endpoint_id;
    uint32_t major_version, minor_version;
    AnomalyIpcSchemaHashV1 request_schema, response_schema, event_schema;
    uint32_t modes;         // AnomalyIpcModeV1 位组合
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

调用方用 selector 声明期望的 `major_version` 与 `minimum_minor_version` 以及三个 schema hash；宿主据此匹配 provider 端点，并在 version / schema 不符时返回相应错误。

## 回调类型

```c
typedef AnomalyStatusV1 (ANOMALY_CALL *AnomalyIpcRequestHandlerV1)(void* user,
    const AnomalyIpcRequestContextV1* context, AnomalyByteSpanV1 request,
    AnomalyMutableByteSpanV1 response, size_t* response_size);
typedef void (ANOMALY_CALL *AnomalyIpcCompletionCallbackV1)(void* user,
    AnomalyGenerationHandleV1 pending_call, AnomalyStatusV1 status, AnomalyByteSpanV1 response);
typedef void (ANOMALY_CALL *AnomalyIpcEventCallbackV1)(void* user,
    AnomalyStringViewV1 endpoint_id, AnomalyByteSpanV1 event);
```

## 服务表

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

| 函数 | 说明 |
| --- | --- |
| `register_endpoint` | 注册 provider 端点，带 request handler；返回 endpoint 句柄 |
| `unregister_endpoint` | 注销端点 |
| `invoke` | 同步请求，`response` 为两段式缓冲 |
| `invoke_async` | 异步请求，完成时回调 `completion`；返回 pending call 句柄 |
| `cancel` | 取消 pending 异步调用 |
| `subscribe` / `unsubscribe` | 订阅 / 取消订阅端点事件 |
| `publish` | provider 端点发布事件 |

> [!NOTE]
> 依赖关系：若 selector 要求的 provider 属于未声明的依赖，宿主可能返回 `ANOMALY_IPC_ERROR_V1_DEPENDENCY_REQUIRED`。所有端点、订阅与 pending call 资源都绑定插件 ID 与 generation，其所有权模型见[架构概览 · 所有权与生命周期](../developer-guide/architecture.md#所有权与生命周期)。
