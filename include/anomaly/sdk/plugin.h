#pragma once
#include "anomaly/sdk/base.h"
#include "anomaly/sdk/services/ui.h"
#include "anomaly/sdk/version.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct AnomalyHostApiV1 {
    uint32_t struct_size; uint16_t api_major; uint16_t api_minor; void* host_context;
    AnomalyAllocatorV1 allocator;
    AnomalyStatusV1 (ANOMALY_CALL *query_service)(void* host_context, AnomalyStringViewV1 service_id, uint32_t minimum_version, const void** service);
} AnomalyHostApiV1;
typedef struct AnomalyPluginDescriptorV1 {
    uint32_t struct_size; uint16_t api_major; uint16_t api_minor;
    AnomalyStringViewV1 id; AnomalyStringViewV1 name; AnomalyStringViewV1 author; AnomalyStringViewV1 version;
    AnomalyStatusV1 (ANOMALY_CALL *on_load)(const AnomalyHostApiV1* host, void** plugin_context);
    AnomalyStatusV1 (ANOMALY_CALL *on_start)(void* plugin_context);
    AnomalyStatusV1 (ANOMALY_CALL *on_stop)(void* plugin_context, uint32_t deadline_milliseconds);
    void (ANOMALY_CALL *on_unload)(void* plugin_context);
    void (ANOMALY_CALL *on_update)(void* plugin_context, double delta_seconds);
    void (ANOMALY_CALL *on_draw)(void* plugin_context, const AnomalyUiServiceV1* ui);
} AnomalyPluginDescriptorV1;
typedef AnomalyStatusV1 (ANOMALY_CALL *AnomalyPluginEntryV1Fn)(AnomalyPluginDescriptorV1* descriptor);
#ifdef __cplusplus
}
#endif
