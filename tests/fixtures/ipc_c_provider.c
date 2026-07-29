#include "anomaly/sdk/anomaly_sdk.h"

#include <string.h>

static const AnomalyIpcServiceV1* g_ipc;
static AnomalyGenerationHandleV1 g_endpoint;

static AnomalyStringViewV1 View(const char* text) {
    AnomalyStringViewV1 result = {text, text == NULL ? 0U : strlen(text)};
    return result;
}

static AnomalyIpcSchemaHashV1 Hash(uint8_t seed) {
    AnomalyIpcSchemaHashV1 result = {{0}};
    size_t index;
    for (index = 0; index < ANOMALY_IPC_SCHEMA_HASH_V1_SIZE; ++index) {
        result.bytes[index] = (uint8_t)(seed + index);
    }
    return result;
}

static AnomalyStatusV1 Status(uint32_t code) {
    AnomalyStatusV1 result = {code, 0, {NULL, 0}};
    return result;
}

static AnomalyStatusV1 ANOMALY_CALL Handle(
    void* user, const AnomalyIpcRequestContextV1* context, AnomalyByteSpanV1 request,
    AnomalyMutableByteSpanV1 response, size_t* response_size) {
    (void)user;
    if (context == NULL || response_size == NULL || request.size != 1U) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    if (response.size < 1U) {
        *response_size = 1U;
        return Status(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL);
    }
    response.data[0] = (uint8_t)(request.data[0] + 1U);
    *response_size = 1U;
    return Status(ANOMALY_STATUS_V1_OK);
}

static AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    const void* service = NULL;
    AnomalyIpcEndpointDescriptorV1 descriptor;
    AnomalyStatusV1 status;
    if (host == NULL || context == NULL || host->query_service == NULL) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    status = host->query_service(
        host->host_context, View(ANOMALY_IPC_SERVICE_V1_ID),
        ANOMALY_IPC_SERVICE_V1_VERSION, &service);
    if (status.code != ANOMALY_STATUS_V1_OK || service == NULL) return status;
    g_ipc = (const AnomalyIpcServiceV1*)service;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.struct_size = sizeof(descriptor);
    descriptor.endpoint_id = View("dev.anomaly.ipc.c-provider");
    descriptor.major_version = 1;
    descriptor.request_schema = Hash(1);
    descriptor.response_schema = Hash(2);
    descriptor.modes = ANOMALY_IPC_MODE_V1_SYNC_REQUEST;
    descriptor.affinity = ANOMALY_IPC_AFFINITY_V1_CALLER;
    descriptor.timeout_milliseconds = 100;
    descriptor.reentrancy = ANOMALY_IPC_REENTRANCY_V1_REJECT;
    descriptor.maximum_request_bytes = 1;
    descriptor.maximum_response_bytes = 1;
    descriptor.maximum_queue_depth = 8;
    status = g_ipc->register_endpoint(
        g_ipc->user, &descriptor, Handle, NULL, &g_endpoint);
    if (status.code == ANOMALY_STATUS_V1_OK) *context = &g_endpoint;
    return status;
}

static AnomalyStatusV1 ANOMALY_CALL Start(void* context) {
    return context == &g_endpoint ? Status(ANOMALY_STATUS_V1_OK)
                                  : Status(ANOMALY_STATUS_V1_FAILED);
}

static AnomalyStatusV1 ANOMALY_CALL Stop(void* context, uint32_t deadline_milliseconds) {
    (void)context;
    (void)deadline_milliseconds;
    return Status(ANOMALY_STATUS_V1_OK);
}

static void ANOMALY_CALL Unload(void* context) {
    (void)context;
    g_ipc = NULL;
    memset(&g_endpoint, 0, sizeof(g_endpoint));
}

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == NULL || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    descriptor->struct_size = sizeof(*descriptor);
    descriptor->api_major = ANOMALY_PLUGIN_API_V1_MAJOR;
    descriptor->api_minor = ANOMALY_PLUGIN_API_V1_MINOR;
    descriptor->id = View("dev.anomaly.ipc-c-provider");
    descriptor->name = View("IPC C Provider");
    descriptor->author = View("Anomaly");
    descriptor->version = View("1.0.0");
    descriptor->on_load = Load;
    descriptor->on_start = Start;
    descriptor->on_stop = Stop;
    descriptor->on_unload = Unload;
    descriptor->on_update = NULL;
    descriptor->on_draw = NULL;
    return Status(ANOMALY_STATUS_V1_OK);
}
