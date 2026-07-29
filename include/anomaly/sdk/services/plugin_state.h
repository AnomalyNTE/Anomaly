#pragma once
#include "anomaly/sdk/base.h"
#define ANOMALY_PLUGIN_STATE_SERVICE_V1_ID "anomaly.plugin-state"
#define ANOMALY_PLUGIN_STATE_SERVICE_V1_VERSION 1u
#ifdef __cplusplus
extern "C" {
#endif
typedef struct AnomalyPluginStateServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *directory)(void* user, char* destination, size_t* inout_size);
} AnomalyPluginStateServiceV1;
#ifdef __cplusplus
}
#endif
