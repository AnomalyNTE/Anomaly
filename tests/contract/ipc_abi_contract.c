#include "anomaly/sdk/services/ipc.h"

#include <stddef.h>

#define ANOMALY_TYPE_IS(expression, type) _Generic((expression), type: 1, default: 0)
#define ANOMALY_ASSERT_OFFSET(type, member, expected) \
    _Static_assert(offsetof(type, member) == (expected), #type "." #member " offset")
#define ANOMALY_ASSERT_LAYOUT(type, expected_size, expected_alignment) \
    _Static_assert(sizeof(type) == (expected_size), #type " size"); \
    _Static_assert(_Alignof(type) == (expected_alignment), #type " alignment")

typedef AnomalyStatusV1 (ANOMALY_CALL *ContractRegisterEndpointFn)(
    void*, const AnomalyIpcEndpointDescriptorV1*, AnomalyIpcRequestHandlerV1, void*,
    AnomalyGenerationHandleV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractHandleFn)(
    void*, AnomalyGenerationHandleV1);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractInvokeFn)(
    void*, const AnomalyIpcEndpointSelectorV1*, AnomalyByteSpanV1,
    AnomalyMutableByteSpanV1, size_t*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractInvokeAsyncFn)(
    void*, const AnomalyIpcEndpointSelectorV1*, AnomalyByteSpanV1,
    AnomalyIpcCompletionCallbackV1, void*, AnomalyGenerationHandleV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractSubscribeFn)(
    void*, const AnomalyIpcEndpointSelectorV1*, AnomalyIpcEventCallbackV1, void*,
    AnomalyGenerationHandleV1*);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractPublishFn)(
    void*, AnomalyGenerationHandleV1, AnomalyByteSpanV1);
typedef AnomalyStatusV1 (ANOMALY_CALL *ContractRequestHandlerFn)(
    void*, const AnomalyIpcRequestContextV1*, AnomalyByteSpanV1,
    AnomalyMutableByteSpanV1, size_t*);
typedef void (ANOMALY_CALL *ContractCompletionFn)(
    void*, AnomalyGenerationHandleV1, AnomalyStatusV1, AnomalyByteSpanV1);
typedef void (ANOMALY_CALL *ContractEventFn)(
    void*, AnomalyStringViewV1, AnomalyByteSpanV1);

_Static_assert(ANOMALY_IPC_SERVICE_V1_VERSION == 1u, "IPC service version");
_Static_assert(ANOMALY_IPC_SCHEMA_HASH_V1_SIZE == 32u, "schema hash size");
_Static_assert(sizeof(AnomalyIpcModeV1) == 4u, "mode enum width");
_Static_assert(sizeof(AnomalyIpcAffinityV1) == 4u, "affinity enum width");
_Static_assert(sizeof(AnomalyIpcReentrancyV1) == 4u, "reentrancy enum width");
_Static_assert(sizeof(AnomalyIpcErrorV1) == 4u, "error enum width");

ANOMALY_ASSERT_LAYOUT(AnomalyIpcSchemaHashV1, 32u, 1u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcSchemaHashV1, bytes, 0u);

ANOMALY_ASSERT_LAYOUT(AnomalyIpcEndpointDescriptorV1, 160u, 8u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcEndpointDescriptorV1, struct_size, 0u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcEndpointDescriptorV1, endpoint_id, 8u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcEndpointDescriptorV1, major_version, 24u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcEndpointDescriptorV1, minor_version, 28u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcEndpointDescriptorV1, request_schema, 32u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcEndpointDescriptorV1, response_schema, 64u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcEndpointDescriptorV1, event_schema, 96u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcEndpointDescriptorV1, modes, 128u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcEndpointDescriptorV1, affinity, 132u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcEndpointDescriptorV1, timeout_milliseconds, 136u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcEndpointDescriptorV1, reentrancy, 140u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcEndpointDescriptorV1, maximum_request_bytes, 144u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcEndpointDescriptorV1, maximum_response_bytes, 148u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcEndpointDescriptorV1, maximum_event_bytes, 152u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcEndpointDescriptorV1, maximum_queue_depth, 156u);

ANOMALY_ASSERT_LAYOUT(AnomalyIpcEndpointSelectorV1, 128u, 8u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcEndpointSelectorV1, struct_size, 0u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcEndpointSelectorV1, endpoint_id, 8u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcEndpointSelectorV1, major_version, 24u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcEndpointSelectorV1, minimum_minor_version, 28u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcEndpointSelectorV1, request_schema, 32u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcEndpointSelectorV1, response_schema, 64u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcEndpointSelectorV1, event_schema, 96u);

ANOMALY_ASSERT_LAYOUT(AnomalyIpcRequestContextV1, 32u, 8u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcRequestContextV1, struct_size, 0u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcRequestContextV1, reserved, 4u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcRequestContextV1, request_id, 8u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcRequestContextV1, caller_plugin_id, 16u);

ANOMALY_ASSERT_LAYOUT(AnomalyIpcServiceV1, 80u, 8u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcServiceV1, struct_size, 0u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcServiceV1, service_version, 4u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcServiceV1, user, 8u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcServiceV1, register_endpoint, 16u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcServiceV1, unregister_endpoint, 24u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcServiceV1, invoke, 32u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcServiceV1, invoke_async, 40u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcServiceV1, cancel, 48u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcServiceV1, subscribe, 56u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcServiceV1, unsubscribe, 64u);
ANOMALY_ASSERT_OFFSET(AnomalyIpcServiceV1, publish, 72u);

_Static_assert(ANOMALY_TYPE_IS((AnomalyIpcRequestHandlerV1)0, ContractRequestHandlerFn),
    "request handler signature");
_Static_assert(ANOMALY_TYPE_IS((AnomalyIpcCompletionCallbackV1)0, ContractCompletionFn),
    "completion signature");
_Static_assert(ANOMALY_TYPE_IS((AnomalyIpcEventCallbackV1)0, ContractEventFn),
    "event callback signature");
_Static_assert(ANOMALY_TYPE_IS(((AnomalyIpcServiceV1*)0)->register_endpoint,
    ContractRegisterEndpointFn), "register endpoint signature");
_Static_assert(ANOMALY_TYPE_IS(((AnomalyIpcServiceV1*)0)->unregister_endpoint,
    ContractHandleFn), "unregister endpoint signature");
_Static_assert(ANOMALY_TYPE_IS(((AnomalyIpcServiceV1*)0)->invoke, ContractInvokeFn),
    "invoke signature");
_Static_assert(ANOMALY_TYPE_IS(((AnomalyIpcServiceV1*)0)->invoke_async,
    ContractInvokeAsyncFn), "invoke async signature");
_Static_assert(ANOMALY_TYPE_IS(((AnomalyIpcServiceV1*)0)->cancel, ContractHandleFn),
    "cancel signature");
_Static_assert(ANOMALY_TYPE_IS(((AnomalyIpcServiceV1*)0)->subscribe,
    ContractSubscribeFn), "subscribe signature");
_Static_assert(ANOMALY_TYPE_IS(((AnomalyIpcServiceV1*)0)->unsubscribe, ContractHandleFn),
    "unsubscribe signature");
_Static_assert(ANOMALY_TYPE_IS(((AnomalyIpcServiceV1*)0)->publish, ContractPublishFn),
    "publish signature");

int main(void) {
    AnomalyIpcEndpointDescriptorV1 descriptor = {0};
    AnomalyIpcEndpointSelectorV1 selector = {0};
    AnomalyIpcServiceV1 service = {0};
    descriptor.struct_size = sizeof(descriptor);
    selector.struct_size = sizeof(selector);
    service.struct_size = sizeof(service);
    return descriptor.struct_size == 0u || selector.struct_size == 0u ||
        service.struct_size == 0u;
}
