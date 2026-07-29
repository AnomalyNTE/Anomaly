#pragma once
#include "anomaly/sdk/base.h"
#define ANOMALY_UE5_BUILD_SERVICE_V1_ID "anomaly.ue5.build"
#define ANOMALY_UE5_BUILD_SERVICE_V1_VERSION 1u
#define ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID "anomaly.ue5.framework"
#define ANOMALY_UE5_FRAMEWORK_SERVICE_V1_VERSION 1u
#define ANOMALY_UE5_NAMES_SERVICE_V1_ID "anomaly.ue5.names"
#define ANOMALY_UE5_NAMES_SERVICE_V1_VERSION 1u
#define ANOMALY_UE5_OBJECTS_SERVICE_V1_ID "anomaly.ue5.objects"
#define ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION 1u
#define ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle) ((uint32_t)((handle).id) - 1u)
#define ANOMALY_UE5_OBJECT_HANDLE_SERIAL(handle) ((uint32_t)((handle).id >> 32u))
#define ANOMALY_UE5_WORLD_SERVICE_V1_ID "anomaly.ue5.world"
#define ANOMALY_UE5_WORLD_SERVICE_V1_VERSION 1u
#ifdef __cplusplus
extern "C" {
#endif
typedef struct AnomalyUe5BuildServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *build_id)(void* user, char* destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *profile_hash)(void* user, char* destination, size_t* inout_size);
    uint32_t (ANOMALY_CALL *feature_state)(void* user, AnomalyStringViewV1 feature_id);
} AnomalyUe5BuildServiceV1;
typedef struct AnomalyUe5FrameworkServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    uint32_t (ANOMALY_CALL *game_thread_id)(void* user);
    uint64_t (ANOMALY_CALL *tick_sequence)(void* user);
    int (ANOMALY_CALL *is_game_thread)(void* user);
} AnomalyUe5FrameworkServiceV1;
typedef struct AnomalyUe5NamesServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *resolve_utf8)(void* user, uint32_t name_id, char* destination, size_t* inout_size);
} AnomalyUe5NamesServiceV1;
typedef struct AnomalyUe5ObjectSnapshotV1 {
    uint32_t struct_size; uint32_t reserved; AnomalyGenerationHandleV1 handle; uint32_t name_id; uint32_t flags;
} AnomalyUe5ObjectSnapshotV1;
typedef struct AnomalyUe5ObjectsServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    uint64_t (ANOMALY_CALL *generation)(void* user);
    uint32_t (ANOMALY_CALL *count)(void* user);
    AnomalyStatusV1 (ANOMALY_CALL *snapshot_at)(void* user, uint32_t index, AnomalyUe5ObjectSnapshotV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *snapshot_by_handle)(void* user, AnomalyGenerationHandleV1 handle, AnomalyUe5ObjectSnapshotV1* snapshot);
} AnomalyUe5ObjectsServiceV1;
typedef struct AnomalyUe5WorldSnapshotV1 {
    uint32_t struct_size; uint32_t reserved; AnomalyGenerationHandleV1 handle; uint64_t change_sequence; uint32_t name_id; uint32_t flags;
} AnomalyUe5WorldSnapshotV1;
typedef struct AnomalyUe5WorldServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *current)(void* user, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *snapshot)(void* user, AnomalyGenerationHandleV1 handle, AnomalyUe5WorldSnapshotV1* snapshot);
} AnomalyUe5WorldServiceV1;
#ifdef __cplusplus
}
#endif
