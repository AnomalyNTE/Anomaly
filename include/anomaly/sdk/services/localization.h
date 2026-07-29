#pragma once

#include "anomaly/sdk/base.h"

#define ANOMALY_LOCALIZATION_SERVICE_V1_ID "anomaly.localization"
#define ANOMALY_LOCALIZATION_SERVICE_V1_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AnomalyLocalizationServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *locale)(
        void* user, char* destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *translate)(
        void* user,
        AnomalyStringViewV1 key,
        AnomalyStringViewV1 english_fallback,
        const AnomalyStringViewV1* arguments,
        size_t argument_count,
        char* destination,
        size_t* inout_size);
} AnomalyLocalizationServiceV1;

#ifdef __cplusplus
}
#endif
