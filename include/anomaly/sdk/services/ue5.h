#pragma once
#include "anomaly/sdk/base.h"
#define ANOMALY_UE5_BUILD_SERVICE_V1_ID "anomaly.ue5.build"
#define ANOMALY_UE5_BUILD_SERVICE_V1_VERSION 1u
#define ANOMALY_UE5_AHUD_SERVICE_V1_ID "anomaly.ue5.ahud"
#define ANOMALY_UE5_AHUD_SERVICE_V1_VERSION 1u
#define ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID "anomaly.ue5.framework"
#define ANOMALY_UE5_FRAMEWORK_SERVICE_V1_VERSION 1u
#define ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID "anomaly.ue5.process-event"
#define ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_VERSION 1u
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
typedef enum AnomalyUe5AhudFrameFlagsV1 {
    ANOMALY_UE5_AHUD_FRAME_V1_NONE = 0
} AnomalyUe5AhudFrameFlagsV1;
typedef struct AnomalyUe5AhudFrameV1 {
    uint32_t struct_size; uint32_t flags; void* user;
    uint32_t viewport_width; uint32_t viewport_height;
    int (ANOMALY_CALL *project)(
        void* user, const double world[3], float screen[2], double* depth);
    int (ANOMALY_CALL *measure_text)(
        void* user, AnomalyStringViewV1 text, float scale, float* width, float* height);
    int (ANOMALY_CALL *draw_text)(
        void* user, AnomalyStringViewV1 text, float x, float y,
        uint32_t color_rgba, float scale);
    int (ANOMALY_CALL *draw_line)(
        void* user, float start_x, float start_y, float end_x, float end_y,
        uint32_t color_rgba, float thickness);
    int (ANOMALY_CALL *draw_rect)(
        void* user, float x, float y, float width, float height,
        uint32_t color_rgba);
} AnomalyUe5AhudFrameV1;
// Calls to the same subscribed endpoint are serialized on the UE Game thread.
// The frame and every function in it are valid only until the callback returns.
typedef void (ANOMALY_CALL *AnomalyUe5AhudDrawCallbackV1)(
    void* user, const AnomalyUe5AhudFrameV1* frame);
typedef struct AnomalyUe5AhudServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *subscribe)(
        void* user, AnomalyUe5AhudDrawCallbackV1 callback, void* callback_user,
        AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *unsubscribe)(
        void* user, AnomalyGenerationHandleV1 handle);
} AnomalyUe5AhudServiceV1;
// A successful unsubscribe prevents future callback admission and normally
// drains an already-entered callback before returning. Self-unsubscribe from
// that callback only closes future admission; callback_user must remain valid
// until the current callback returns.
typedef struct AnomalyUe5FrameworkServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    uint32_t (ANOMALY_CALL *game_thread_id)(void* user);
    uint64_t (ANOMALY_CALL *tick_sequence)(void* user);
    int (ANOMALY_CALL *is_game_thread)(void* user);
} AnomalyUe5FrameworkServiceV1;
// One host UObject::ProcessEvent hook fans out to all subscribers. Subscribers
// run serially on the UE Game thread before the original function. The
// parameter buffer is valid only for the duration of the callback, may be null
// for a zero-parameter function, and may be adjusted in place.
typedef void (ANOMALY_CALL *AnomalyUe5ProcessEventCallbackV1)(
    void* user, uintptr_t object, uintptr_t function, void* parameters);
typedef struct AnomalyUe5ProcessEventServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *subscribe)(
        void* user, AnomalyUe5ProcessEventCallbackV1 callback,
        void* callback_user, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *unsubscribe)(
        void* user, AnomalyGenerationHandleV1 handle);
} AnomalyUe5ProcessEventServiceV1;
typedef struct AnomalyUe5NamesServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *resolve_utf8)(void* user, uint32_t name_id, char* destination, size_t* inout_size);
    // Resolves an in-process UE FText value to UTF-8. The address is valid only
    // for the current Game-thread snapshot and is never retained by the Host.
    // Reads Profile-validated cached display text or loaded string-table source
    // text. Unsupported history returns NOT_FOUND; no UE text function is called.
    AnomalyStatusV1 (ANOMALY_CALL *resolve_ftext_utf8)(
        void* user, uintptr_t ftext_address, char* destination, size_t* inout_size);
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
    AnomalyStatusV1 (ANOMALY_CALL *find_exact)(void* user, AnomalyStringViewV1 path, AnomalyGenerationHandleV1* handle);
} AnomalyUe5ObjectsServiceV1;
typedef struct AnomalyUe5WorldSnapshotV1 {
    uint32_t struct_size; uint32_t reserved; AnomalyGenerationHandleV1 handle; uint64_t change_sequence; uint32_t name_id; uint32_t flags;
} AnomalyUe5WorldSnapshotV1;
typedef struct AnomalyUe5WorldServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *current)(void* user, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *snapshot)(void* user, AnomalyGenerationHandleV1 handle, AnomalyUe5WorldSnapshotV1* snapshot);
} AnomalyUe5WorldServiceV1;

#define ANOMALY_UE5_STREAMING_SOURCE_SERVICE_V1_ID "anomaly.ue5.streaming-source"
#define ANOMALY_UE5_STREAMING_SOURCE_SERVICE_V1_VERSION 1u
#define ANOMALY_UE5_STREAMING_SOURCE_DURATION_V1_UNTIL_CLEARED 0u
// Redirects world streaming: while an override is active the local player controller's
// streaming source is evaluated at the requested position instead of at the pawn, which
// makes that area load before anything is moved there. The Host owns the single
// process-wide streaming-source hook, so any number of consumers share it. Consumers share
// one override slot, so the most recent request wins until it expires or is cleared.
typedef enum AnomalyUe5StreamingSourceOverrideFlagsV1 {
    ANOMALY_UE5_STREAMING_SOURCE_OVERRIDE_V1_NONE = 0,
    // Also redirect the streaming source rotation, as a free camera following the view does.
    ANOMALY_UE5_STREAMING_SOURCE_OVERRIDE_V1_ROTATION = 1
} AnomalyUe5StreamingSourceOverrideFlagsV1;
typedef struct AnomalyUe5StreamingSourceOverrideV1 {
    uint32_t struct_size; uint32_t flags;
    double position[3];
    // Used only with ANOMALY_UE5_STREAMING_SOURCE_OVERRIDE_V1_ROTATION.
    double rotation[3];
    // ANOMALY_UE5_STREAMING_SOURCE_DURATION_V1_UNTIL_CLEARED keeps the override until
    // clear_override. Any other value expires it that many milliseconds after the call,
    // which bounds the request even when the consumer never clears it.
    uint32_t duration_milliseconds; uint32_t reserved;
} AnomalyUe5StreamingSourceOverrideV1;
typedef struct AnomalyUe5StreamingSourceServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    // Valid only from the Game callback domain. Returns UNAVAILABLE when the active
    // Profile does not carry a validated streaming source, and INVALID_ARGUMENT when the
    // position is not finite.
    AnomalyStatusV1 (ANOMALY_CALL *set_override)(void* user, const AnomalyUe5StreamingSourceOverrideV1* request);
    AnomalyStatusV1 (ANOMALY_CALL *clear_override)(void* user);
} AnomalyUe5StreamingSourceServiceV1;
#ifdef __cplusplus
}
#endif
