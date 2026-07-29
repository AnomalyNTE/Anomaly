#pragma once
#include "anomaly/sdk/base.h"
#define ANOMALY_CORE_SERVICE_V1_ID "anomaly.core"
#define ANOMALY_CORE_SERVICE_V1_VERSION 1u
#ifdef __cplusplus
extern "C" {
#endif
typedef enum AnomalyCoreLogLevelV1 {
    ANOMALY_CORE_LOG_LEVEL_V1_TRACE = 0,
    ANOMALY_CORE_LOG_LEVEL_V1_INFO = 1,
    ANOMALY_CORE_LOG_LEVEL_V1_WARNING = 2,
    ANOMALY_CORE_LOG_LEVEL_V1_ERROR = 3
} AnomalyCoreLogLevelV1;

typedef struct AnomalyCoreServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    void (ANOMALY_CALL *log)(void* user, uint32_t level, AnomalyStringViewV1 message);
    AnomalyStatusV1 (ANOMALY_CALL *read_memory)(void* user, uintptr_t address, AnomalyMutableByteSpanV1 destination);
    AnomalyStatusV1 (ANOMALY_CALL *write_memory)(void* user, uintptr_t address, AnomalyByteSpanV1 source);
    AnomalyStatusV1 (ANOMALY_CALL *plugin_directory)(void* user, char* destination, size_t* inout_size);
} AnomalyCoreServiceV1;
#ifdef __cplusplus
}
#endif
