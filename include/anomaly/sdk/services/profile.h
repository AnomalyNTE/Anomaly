#pragma once
#include "anomaly/sdk/base.h"

#define ANOMALY_PROFILE_SERVICE_V1_ID "anomaly.profile"
#define ANOMALY_PROFILE_SERVICE_V1_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

// Reads profile/<relative_path> from this plugin's immutable package generation
// and validates plugin-profile.schema.json. Lifecycle/Worker only. The returned
// document is an anomaly.json handle; inspect and release it through that service.
// Both services require the json capability. No paths outside profile/ are allowed.
typedef struct AnomalyProfileServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *load)(
        void* user, AnomalyStringViewV1 relative_path,
        AnomalyGenerationHandleV1* document);
} AnomalyProfileServiceV1;

#ifdef __cplusplus
}
#endif
