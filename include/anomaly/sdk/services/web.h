#pragma once

#include "anomaly/sdk/base.h"

#define ANOMALY_WEB_SERVICE_V1_ID "anomaly.web"
#define ANOMALY_WEB_SERVICE_V1_VERSION 1u
#define ANOMALY_WEB_MAX_REQUEST_BYTES_V1 65536u
#define ANOMALY_WEB_MAX_RESPONSE_BYTES_V1 1048576u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AnomalyWebContentTypeV1 {
    ANOMALY_WEB_CONTENT_TYPE_V1_JSON = 0,
    ANOMALY_WEB_CONTENT_TYPE_V1_TEXT = 1
} AnomalyWebContentTypeV1;

typedef struct AnomalyWebRequestV1 {
    uint32_t struct_size;
    uint32_t reserved;
    AnomalyStringViewV1 method;
    // Application-relative path, including a leading slash (for example /api/state).
    AnomalyStringViewV1 path;
    AnomalyStringViewV1 body;
} AnomalyWebRequestV1;

typedef struct AnomalyWebResponseV1 {
    uint32_t struct_size;
    uint32_t http_status;
    uint32_t content_type;
    uint32_t reserved;
    // Host-owned writable storage; capacity is MAX_RESPONSE_BYTES. Do not replace it.
    AnomalyMutableByteSpanV1 body;
    size_t body_size;
} AnomalyWebResponseV1;

// Invoked once per authenticated API request on Runtime Worker, with a generation
// lease. Copy response bytes before returning. Game work must be enqueued for on_update.
typedef AnomalyStatusV1 (ANOMALY_CALL *AnomalyWebCallbackV1)(
    void* user, const AnomalyWebRequestV1* request, AnomalyWebResponseV1* response);

typedef struct AnomalyWebServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    // Lifecycle/Worker only. Assets are relative to this DLL generation's package,
    // preloaded by Runtime Worker. One application per plugin generation.
    AnomalyStatusV1 (ANOMALY_CALL *register_application)(
        void* user, AnomalyStringViewV1 assets_relative_directory,
        AnomalyWebCallbackV1 callback, void* callback_user,
        AnomalyGenerationHandleV1* application);
    AnomalyStatusV1 (ANOMALY_CALL *unregister_application)(
        void* user, AnomalyGenerationHandleV1 application);
    // Two-call NUL-terminated string protocol. URL includes the session token in
    // #token=...; API callers send it as X-Anomaly-Token. Never persist this URL.
    AnomalyStatusV1 (ANOMALY_CALL *application_url)(
        void* user, AnomalyGenerationHandleV1 application,
        char* destination, size_t* inout_size);
} AnomalyWebServiceV1;

#ifdef __cplusplus
}
#endif
