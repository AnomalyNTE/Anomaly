#include "anomaly/sdk/anomaly_sdk.h"

#include <string.h>

static const AnomalyIpcServiceV1* g_ipc;

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

static AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    const void* service = NULL;
    AnomalyStatusV1 status;
    if (host == NULL || context == NULL || host->query_service == NULL) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    status = host->query_service(
        host->host_context, View(ANOMALY_IPC_SERVICE_V1_ID),
        ANOMALY_IPC_SERVICE_V1_VERSION, &service);
    if (status.code != ANOMALY_STATUS_V1_OK || service == NULL) return status;
    g_ipc = (const AnomalyIpcServiceV1*)service;
    *context = (void*)g_ipc;
    return Status(ANOMALY_STATUS_V1_OK);
}

static AnomalyStatusV1 ANOMALY_CALL Start(void* context) {
    AnomalyIpcEndpointSelectorV1 selector;
    const uint8_t request = 0x5aU;
    uint8_t response = 0;
    size_t response_size = 1;
    AnomalyStatusV1 status;
    if (context != (void*)g_ipc) return Status(ANOMALY_STATUS_V1_FAILED);
    memset(&selector, 0, sizeof(selector));
    selector.struct_size = sizeof(selector);
    selector.endpoint_id = View("dev.anomaly.ipc.cpp-provider");
    selector.major_version = 1;
    selector.request_schema = Hash(11);
    selector.response_schema = Hash(12);
    status = g_ipc->invoke(
        g_ipc->user, &selector, (AnomalyByteSpanV1){&request, 1},
        (AnomalyMutableByteSpanV1){&response, 1}, &response_size);
    return status.code == ANOMALY_STATUS_V1_OK && response_size == 1 && response == 0xa5U
        ? status : Status(ANOMALY_STATUS_V1_FAILED);
}

static AnomalyStatusV1 ANOMALY_CALL Stop(void* context, uint32_t deadline_milliseconds) {
    (void)context;
    (void)deadline_milliseconds;
    return Status(ANOMALY_STATUS_V1_OK);
}

static void ANOMALY_CALL Unload(void* context) {
    (void)context;
    g_ipc = NULL;
}

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == NULL || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    descriptor->struct_size = sizeof(*descriptor);
    descriptor->api_major = ANOMALY_PLUGIN_API_V1_MAJOR;
    descriptor->api_minor = ANOMALY_PLUGIN_API_V1_MINOR;
    descriptor->id = View("dev.anomaly.ipc-c-consumer");
    descriptor->name = View("IPC C Consumer");
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
