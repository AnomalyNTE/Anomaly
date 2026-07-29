#pragma once

#include "anomaly/sdk/base.h"

#define ANOMALY_SIGNATURE_SERVICE_V1_ID "anomaly.interop.signature"
#define ANOMALY_SIGNATURE_SERVICE_V1_VERSION 1u
#define ANOMALY_HOOK_SERVICE_V1_ID "anomaly.interop.hook"
#define ANOMALY_HOOK_SERVICE_V1_VERSION 1u
#define ANOMALY_PATCH_SERVICE_V1_ID "anomaly.interop.patch"
#define ANOMALY_PATCH_SERVICE_V1_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AnomalyHookKindV1 {
    ANOMALY_HOOK_V1_FUNCTION = 1,
    ANOMALY_HOOK_V1_IAT = 2,
    ANOMALY_HOOK_V1_EXPORT = 3,
    ANOMALY_HOOK_V1_VTABLE = 4
} AnomalyHookKindV1;

typedef struct AnomalyHookRequestV1 {
    uint32_t struct_size;
    uint32_t kind;
    uintptr_t target;
    void* detour;
    AnomalyStringViewV1 label;
} AnomalyHookRequestV1;

typedef struct AnomalySignatureServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *resolve)(
        void* user,
        AnomalyStringViewV1 module_name,
        AnomalyStringViewV1 section_name,
        AnomalyStringViewV1 pattern,
        uintptr_t* address);
} AnomalySignatureServiceV1;

typedef struct AnomalyHookServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *create)(
        void* user,
        const AnomalyHookRequestV1* request,
        uintptr_t* original,
        AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *release)(
        void* user, AnomalyGenerationHandleV1 handle);
    AnomalyStatusV1 (ANOMALY_CALL *begin_callback)(
        void* user,
        AnomalyGenerationHandleV1 hook,
        AnomalyGenerationHandleV1* callback_lease);
    AnomalyStatusV1 (ANOMALY_CALL *end_callback)(
        void* user, AnomalyGenerationHandleV1 callback_lease);
} AnomalyHookServiceV1;

typedef struct AnomalyPatchServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *apply)(
        void* user,
        uintptr_t address,
        AnomalyByteSpanV1 replacement,
        AnomalyStringViewV1 label,
        AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *release)(
        void* user, AnomalyGenerationHandleV1 handle);
} AnomalyPatchServiceV1;

#ifdef __cplusplus
}
#endif
