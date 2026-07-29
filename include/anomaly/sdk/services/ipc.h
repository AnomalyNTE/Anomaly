#pragma once

#include "anomaly/sdk/base.h"

#define ANOMALY_IPC_SERVICE_V1_ID "anomaly.ipc"
#define ANOMALY_IPC_SERVICE_V1_VERSION 1u
#define ANOMALY_IPC_SCHEMA_HASH_V1_SIZE 32u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AnomalyIpcModeV1 {
    ANOMALY_IPC_MODE_V1_SYNC_REQUEST = 1u << 0u,
    ANOMALY_IPC_MODE_V1_ASYNC_REQUEST = 1u << 1u,
    ANOMALY_IPC_MODE_V1_EVENT = 1u << 2u
} AnomalyIpcModeV1;

typedef enum AnomalyIpcAffinityV1 {
    ANOMALY_IPC_AFFINITY_V1_CALLER = 0,
    ANOMALY_IPC_AFFINITY_V1_WORKER = 1,
    ANOMALY_IPC_AFFINITY_V1_LIFECYCLE = 2,
    ANOMALY_IPC_AFFINITY_V1_GAME = 3,
    ANOMALY_IPC_AFFINITY_V1_RENDER = 4
} AnomalyIpcAffinityV1;

typedef enum AnomalyIpcReentrancyV1 {
    ANOMALY_IPC_REENTRANCY_V1_REJECT = 0,
    ANOMALY_IPC_REENTRANCY_V1_ALLOW = 1
} AnomalyIpcReentrancyV1;

typedef enum AnomalyIpcErrorV1 {
    ANOMALY_IPC_ERROR_V1_NONE = 0,
    ANOMALY_IPC_ERROR_V1_PROVIDER_MISSING = 1,
    ANOMALY_IPC_ERROR_V1_VERSION_MISMATCH = 2,
    ANOMALY_IPC_ERROR_V1_SCHEMA_MISMATCH = 3,
    ANOMALY_IPC_ERROR_V1_MODE_UNAVAILABLE = 4,
    ANOMALY_IPC_ERROR_V1_TIMEOUT = 5,
    ANOMALY_IPC_ERROR_V1_REENTRANT_CYCLE = 6,
    ANOMALY_IPC_ERROR_V1_QUEUE_FULL = 7,
    ANOMALY_IPC_ERROR_V1_STALE_GENERATION = 8,
    ANOMALY_IPC_ERROR_V1_DEPENDENCY_REQUIRED = 9
} AnomalyIpcErrorV1;

typedef struct AnomalyIpcSchemaHashV1 {
    uint8_t bytes[ANOMALY_IPC_SCHEMA_HASH_V1_SIZE];
} AnomalyIpcSchemaHashV1;

typedef struct AnomalyIpcEndpointDescriptorV1 {
    uint32_t struct_size;
    AnomalyStringViewV1 endpoint_id;
    uint32_t major_version;
    uint32_t minor_version;
    AnomalyIpcSchemaHashV1 request_schema;
    AnomalyIpcSchemaHashV1 response_schema;
    AnomalyIpcSchemaHashV1 event_schema;
    uint32_t modes;
    uint32_t affinity;
    uint32_t timeout_milliseconds;
    uint32_t reentrancy;
    uint32_t maximum_request_bytes;
    uint32_t maximum_response_bytes;
    uint32_t maximum_event_bytes;
    uint32_t maximum_queue_depth;
} AnomalyIpcEndpointDescriptorV1;

typedef struct AnomalyIpcEndpointSelectorV1 {
    uint32_t struct_size;
    AnomalyStringViewV1 endpoint_id;
    uint32_t major_version;
    uint32_t minimum_minor_version;
    AnomalyIpcSchemaHashV1 request_schema;
    AnomalyIpcSchemaHashV1 response_schema;
    AnomalyIpcSchemaHashV1 event_schema;
} AnomalyIpcEndpointSelectorV1;

typedef struct AnomalyIpcRequestContextV1 {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t request_id;
    AnomalyStringViewV1 caller_plugin_id;
} AnomalyIpcRequestContextV1;

typedef AnomalyStatusV1 (ANOMALY_CALL *AnomalyIpcRequestHandlerV1)(
    void* user,
    const AnomalyIpcRequestContextV1* context,
    AnomalyByteSpanV1 request,
    AnomalyMutableByteSpanV1 response,
    size_t* response_size);

typedef void (ANOMALY_CALL *AnomalyIpcCompletionCallbackV1)(
    void* user,
    AnomalyGenerationHandleV1 pending_call,
    AnomalyStatusV1 status,
    AnomalyByteSpanV1 response);

typedef void (ANOMALY_CALL *AnomalyIpcEventCallbackV1)(
    void* user,
    AnomalyStringViewV1 endpoint_id,
    AnomalyByteSpanV1 event);

typedef struct AnomalyIpcServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *register_endpoint)(
        void* user,
        const AnomalyIpcEndpointDescriptorV1* descriptor,
        AnomalyIpcRequestHandlerV1 request_handler,
        void* callback_user,
        AnomalyGenerationHandleV1* endpoint);
    AnomalyStatusV1 (ANOMALY_CALL *unregister_endpoint)(
        void* user, AnomalyGenerationHandleV1 endpoint);
    AnomalyStatusV1 (ANOMALY_CALL *invoke)(
        void* user,
        const AnomalyIpcEndpointSelectorV1* selector,
        AnomalyByteSpanV1 request,
        AnomalyMutableByteSpanV1 response,
        size_t* response_size);
    AnomalyStatusV1 (ANOMALY_CALL *invoke_async)(
        void* user,
        const AnomalyIpcEndpointSelectorV1* selector,
        AnomalyByteSpanV1 request,
        AnomalyIpcCompletionCallbackV1 completion,
        void* completion_user,
        AnomalyGenerationHandleV1* pending_call);
    AnomalyStatusV1 (ANOMALY_CALL *cancel)(
        void* user, AnomalyGenerationHandleV1 pending_call);
    AnomalyStatusV1 (ANOMALY_CALL *subscribe)(
        void* user,
        const AnomalyIpcEndpointSelectorV1* selector,
        AnomalyIpcEventCallbackV1 callback,
        void* callback_user,
        AnomalyGenerationHandleV1* subscription);
    AnomalyStatusV1 (ANOMALY_CALL *unsubscribe)(
        void* user, AnomalyGenerationHandleV1 subscription);
    AnomalyStatusV1 (ANOMALY_CALL *publish)(
        void* user,
        AnomalyGenerationHandleV1 endpoint,
        AnomalyByteSpanV1 event);
} AnomalyIpcServiceV1;

#ifdef __cplusplus
}
#endif
