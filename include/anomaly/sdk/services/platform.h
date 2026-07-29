#pragma once

#include "anomaly/sdk/base.h"

#define ANOMALY_CONFIG_SERVICE_V1_ID "anomaly.config"
#define ANOMALY_CONFIG_SERVICE_V1_VERSION 1u
#define ANOMALY_STORAGE_SERVICE_V1_ID "anomaly.storage"
#define ANOMALY_STORAGE_SERVICE_V1_VERSION 1u
#define ANOMALY_RUNTIME_INFO_SERVICE_V1_ID "anomaly.runtime-info"
#define ANOMALY_RUNTIME_INFO_SERVICE_V1_VERSION 1u
#define ANOMALY_DIAGNOSTICS_SERVICE_V1_ID "anomaly.diagnostics"
#define ANOMALY_DIAGNOSTICS_SERVICE_V1_VERSION 1u
#define ANOMALY_SCHEDULER_SERVICE_V1_ID "anomaly.scheduler"
#define ANOMALY_SCHEDULER_SERVICE_V1_VERSION 1u
#define ANOMALY_COMMANDS_SERVICE_V1_ID "anomaly.commands"
#define ANOMALY_COMMANDS_SERVICE_V1_VERSION 1u
#define ANOMALY_NOTIFICATIONS_SERVICE_V1_ID "anomaly.notifications"
#define ANOMALY_NOTIFICATIONS_SERVICE_V1_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AnomalyNotificationSeverityV1 {
    ANOMALY_NOTIFICATION_V1_INFO = 0,
    ANOMALY_NOTIFICATION_V1_WARNING = 1,
    ANOMALY_NOTIFICATION_V1_ERROR = 2
} AnomalyNotificationSeverityV1;

typedef struct AnomalyRuntimeInfoV1 {
    uint32_t struct_size;
    uint32_t runtime_version_major;
    uint32_t runtime_version_minor;
    uint32_t runtime_version_patch;
    uint32_t process_id;
    uint32_t thread_id;
    uint64_t uptime_milliseconds;
    uint64_t plugin_generation;
} AnomalyRuntimeInfoV1;

typedef AnomalyStatusV1 (ANOMALY_CALL *AnomalyConfigMigrationV1)(
    void* user,
    uint32_t source_schema_version,
    AnomalyByteSpanV1 source,
    AnomalyMutableByteSpanV1 destination,
    size_t* inout_size);

typedef AnomalyStatusV1 (ANOMALY_CALL *AnomalyDiagnosticSelfTestV1)(
    void* user, AnomalyMutableByteSpanV1 destination, size_t* inout_size);

typedef void (ANOMALY_CALL *AnomalyTaskCallbackV1)(
    void* user, AnomalyGenerationHandleV1 task);

typedef AnomalyStatusV1 (ANOMALY_CALL *AnomalyCommandCallbackV1)(
    void* user,
    AnomalyStringViewV1 arguments,
    AnomalyMutableByteSpanV1 destination,
    size_t* inout_size);

typedef struct AnomalyConfigServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    // Register the JSON Schema for one plugin-local document. A plugin must
    // register its stable schema_id in every loaded generation before calling
    // read, write_atomic, or migrate. The returned handle is scope-owned.
    AnomalyStatusV1 (ANOMALY_CALL *register_schema)(
        void* user,
        AnomalyStringViewV1 schema_id,
        uint32_t schema_version,
        AnomalyByteSpanV1 schema_json,
        AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *unregister_schema)(
        void* user, AnomalyGenerationHandleV1 handle);
    // Read the persisted JSON document through the standard two-call buffer
    // protocol. ANOMALY_STATUS_V1_NOT_FOUND means this plugin has no saved
    // document yet; the returned schema_version is the stored version.
    AnomalyStatusV1 (ANOMALY_CALL *read)(
        void* user,
        AnomalyStringViewV1 schema_id,
        uint32_t* schema_version,
        AnomalyMutableByteSpanV1 destination,
        size_t* inout_size);
    // Validate against the registered schema and atomically replace the
    // plugin-local document. The Host owns the durable state location.
    AnomalyStatusV1 (ANOMALY_CALL *write_atomic)(
        void* user,
        AnomalyStringViewV1 schema_id,
        uint32_t schema_version,
        AnomalyByteSpanV1 document);
    AnomalyStatusV1 (ANOMALY_CALL *migrate)(
        void* user,
        AnomalyStringViewV1 schema_id,
        AnomalyConfigMigrationV1 migration,
        void* migration_user);
} AnomalyConfigServiceV1;

typedef struct AnomalyStorageServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *read)(
        void* user,
        AnomalyStringViewV1 relative_path,
        AnomalyMutableByteSpanV1 destination,
        size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *write_atomic)(
        void* user, AnomalyStringViewV1 relative_path, AnomalyByteSpanV1 source);
    AnomalyStatusV1 (ANOMALY_CALL *remove)(
        void* user, AnomalyStringViewV1 relative_path);
} AnomalyStorageServiceV1;

typedef struct AnomalyRuntimeInfoServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *snapshot)(
        void* user, AnomalyRuntimeInfoV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *runtime_version_utf8)(
        void* user, char* destination, size_t* inout_size);
} AnomalyRuntimeInfoServiceV1;

typedef struct AnomalyDiagnosticsServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *register_self_test)(
        void* user,
        AnomalyStringViewV1 id,
        AnomalyDiagnosticSelfTestV1 callback,
        void* callback_user,
        AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *unregister_self_test)(
        void* user, AnomalyGenerationHandleV1 handle);
    AnomalyStatusV1 (ANOMALY_CALL *run_self_test)(
        void* user,
        AnomalyStringViewV1 id,
        AnomalyMutableByteSpanV1 destination,
        size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *snapshot_json)(
        void* user, AnomalyMutableByteSpanV1 destination, size_t* inout_size);
} AnomalyDiagnosticsServiceV1;

typedef struct AnomalySchedulerServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *schedule)(
        void* user,
        uint32_t delay_milliseconds,
        AnomalyTaskCallbackV1 callback,
        void* callback_user,
        AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *cancel)(
        void* user, AnomalyGenerationHandleV1 handle);
} AnomalySchedulerServiceV1;

typedef struct AnomalyCommandsServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *register_command)(
        void* user,
        AnomalyStringViewV1 name,
        AnomalyStringViewV1 description,
        AnomalyCommandCallbackV1 callback,
        void* callback_user,
        AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *unregister_command)(
        void* user, AnomalyGenerationHandleV1 handle);
    AnomalyStatusV1 (ANOMALY_CALL *invoke)(
        void* user,
        AnomalyStringViewV1 name,
        AnomalyStringViewV1 arguments,
        AnomalyMutableByteSpanV1 destination,
        size_t* inout_size);
} AnomalyCommandsServiceV1;

typedef struct AnomalyNotificationsServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *post)(
        void* user,
        AnomalyNotificationSeverityV1 severity,
        AnomalyStringViewV1 title,
        AnomalyStringViewV1 body,
        uint32_t timeout_milliseconds,
        AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *dismiss)(
        void* user, AnomalyGenerationHandleV1 handle);
} AnomalyNotificationsServiceV1;

#ifdef __cplusplus
}
#endif
