#pragma once
#include "anomaly/sdk/base.h"
#define ANOMALY_NTE_BUILD_SERVICE_V1_ID "anomaly.nte.build"
#define ANOMALY_NTE_BUILD_SERVICE_V1_VERSION 1u
#define ANOMALY_NTE_SESSION_SERVICE_V1_ID "anomaly.nte.session"
#define ANOMALY_NTE_SESSION_SERVICE_V1_VERSION 1u
#define ANOMALY_NTE_PLAYER_SERVICE_V1_ID "anomaly.nte.player"
#define ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION 1u
#define ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID "anomaly.nte.player-teleport"
#define ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION 1u
#define ANOMALY_NTE_ENTITIES_SERVICE_V1_ID "anomaly.nte.entities"
#define ANOMALY_NTE_ENTITIES_SERVICE_V1_VERSION 1u
#define ANOMALY_NTE_ACTORS_SERVICE_V1_ID "anomaly.nte.actors"
#define ANOMALY_NTE_ACTORS_SERVICE_V1_VERSION 1u
#define ANOMALY_NTE_ENTITY_PAGE_V1_MAX_CAPACITY 256u
#define ANOMALY_NTE_METRICS_SERVICE_V1_ID "anomaly.nte.metrics"
#define ANOMALY_NTE_METRICS_SERVICE_V1_VERSION 1u
#define ANOMALY_NTE_ESC_MENU_BUTTON_SERVICE_V1_ID "anomaly.nte.esc-menu-button"
#define ANOMALY_NTE_ESC_MENU_BUTTON_SERVICE_V1_VERSION 1u
// Service tables belong to one Host lifecycle generation. A cached table from a stopped or
// replaced generation remains callable only to report UNAVAILABLE (or zero for scalar queries);
// it never resumes against a later Start generation.
#ifdef __cplusplus
extern "C" {
#endif

typedef enum AnomalyNteEscMenuButtonFlagsV1 {
    ANOMALY_NTE_ESC_MENU_BUTTON_V1_NONE = 0
} AnomalyNteEscMenuButtonFlagsV1;

typedef enum AnomalyNteEscMenuButtonIconFormatV1 {
    ANOMALY_NTE_ESC_MENU_BUTTON_ICON_V1_NONE = 0,
    ANOMALY_NTE_ESC_MENU_BUTTON_ICON_V1_PNG = 1
} AnomalyNteEscMenuButtonIconFormatV1;

typedef enum AnomalyNteEscMenuButtonResultV1 {
    ANOMALY_NTE_ESC_MENU_BUTTON_RESULT_V1_NONE = 0,
    ANOMALY_NTE_ESC_MENU_BUTTON_RESULT_V1_EXPAND_ANOMALY = 1
} AnomalyNteEscMenuButtonResultV1;

typedef struct AnomalyNteEscMenuButtonSpecV1 {
    uint32_t struct_size;
    uint32_t flags;
    AnomalyStringViewV1 id;
    AnomalyStringViewV1 label;
    uint32_t icon_format;
    uint32_t reserved;
    AnomalyByteSpanV1 icon_bytes;
} AnomalyNteEscMenuButtonSpecV1;

typedef uint32_t (ANOMALY_CALL *AnomalyNteEscMenuButtonCallbackV1)(
    void* user, AnomalyGenerationHandleV1 button);

typedef struct AnomalyNteEscMenuButtonServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *register_button)(
        void* user, const AnomalyNteEscMenuButtonSpecV1* spec,
        AnomalyNteEscMenuButtonCallbackV1 callback, void* callback_user,
        AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *unregister_button)(
        void* user, AnomalyGenerationHandleV1 handle);
} AnomalyNteEscMenuButtonServiceV1;
typedef struct AnomalyNteBuildServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *build_id)(void* user, char* destination, size_t* inout_size);
    uint32_t (ANOMALY_CALL *feature_state)(void* user, AnomalyStringViewV1 feature_id);
} AnomalyNteBuildServiceV1;
typedef enum AnomalyNteSessionStateV1 {
    ANOMALY_NTE_SESSION_V1_UNKNOWN = 0,
    ANOMALY_NTE_SESSION_V1_LOADING = 1,
    ANOMALY_NTE_SESSION_V1_WORLD_READY = 2
} AnomalyNteSessionStateV1;
typedef struct AnomalyNteSessionSnapshotV1 {
    uint32_t struct_size; uint32_t state; uint64_t sequence; AnomalyGenerationHandleV1 world;
} AnomalyNteSessionSnapshotV1;
// The pull-based lifecycle event stream never exposes a World pointer;
// callers retain only an opaque, monotonically increasing event cursor and generation handles.
// The Host reserves a discontinuity between lifecycle generations, so non-zero cursors do not
// survive a Host lifecycle restart.
typedef enum AnomalyNteSessionEventKindV1 {
    ANOMALY_NTE_SESSION_EVENT_V1_NONE = 0,
    ANOMALY_NTE_SESSION_EVENT_V1_WORLD_READY = 1,
    ANOMALY_NTE_SESSION_EVENT_V1_WORLD_CHANGED = 2,
    ANOMALY_NTE_SESSION_EVENT_V1_WORLD_UNAVAILABLE = 3
} AnomalyNteSessionEventKindV1;
typedef struct AnomalyNteSessionEventV1 {
    uint32_t struct_size; uint32_t kind;
    uint64_t sequence; uint64_t tick_sequence;
    AnomalyGenerationHandleV1 previous_world;
    AnomalyGenerationHandleV1 world;
} AnomalyNteSessionEventV1;
typedef struct AnomalyNteSessionServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *snapshot)(void* user, AnomalyNteSessionSnapshotV1* snapshot);
    // Returns the first retained event with sequence greater than after_sequence. A stale
    // non-zero cursor and an empty future range both return ANOMALY_STATUS_V1_NOT_FOUND.
    AnomalyStatusV1 (ANOMALY_CALL *next_event)(void* user, uint64_t after_sequence,
        AnomalyNteSessionEventV1* event);
    uint64_t (ANOMALY_CALL *latest_event_sequence)(void* user);
} AnomalyNteSessionServiceV1;

typedef uint32_t AnomalyNteSnapshotFlagsV1;
#define ANOMALY_NTE_SNAPSHOT_V1_INVALID 0u
#define ANOMALY_NTE_SNAPSHOT_V1_VALID (1u << 29u)
#define ANOMALY_NTE_SNAPSHOT_V1_STALE (1u << 30u)
#define ANOMALY_NTE_SNAPSHOT_V1_PARTIAL (1u << 31u)
typedef struct AnomalyNtePlayerSnapshotV1 {
    uint32_t struct_size; uint32_t flags; AnomalyGenerationHandleV1 handle; uint64_t sequence; double position[3];
} AnomalyNtePlayerSnapshotV1;
typedef struct AnomalyNtePlayerEspSnapshotV1 {
    uint32_t struct_size; uint32_t flags; AnomalyGenerationHandleV1 handle; uint64_t sequence;
    double bounds_center[3]; double bounds_extent[3];
    double camera_position[3]; double camera_rotation[3];
    float horizontal_fov_degrees; uint32_t reserved;
} AnomalyNtePlayerEspSnapshotV1;
// Camera data is published only when the active Profile has validated the Player
// service's optional nte.player-esp capability.
// world identifies the scene and player identifies the Pawn/Controller sample that supplied
// this camera. Either generation handle becoming stale invalidates the corresponding relation.
typedef struct AnomalyNteCameraSnapshotV1 {
    uint32_t struct_size; uint32_t flags;
    AnomalyGenerationHandleV1 world; AnomalyGenerationHandleV1 player;
    uint64_t sequence;
    double position[3]; double rotation[3];
    float horizontal_fov_degrees; uint32_t reserved;
} AnomalyNteCameraSnapshotV1;
typedef struct AnomalyNtePlayerServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *snapshot)(void* user, AnomalyNtePlayerSnapshotV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *esp_snapshot)(void* user, AnomalyNtePlayerEspSnapshotV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *camera_snapshot)(void* user,
        AnomalyNteCameraSnapshotV1* snapshot);
} AnomalyNtePlayerServiceV1;

// Requests a teleport from a Host that has published the validated engine-owned teleport bridge.
// A plugin must declare the explicit nte-player-teleport capability.
// Fingerprint nte-win64-e63ff9c7-10008000-19bb677e6b863805 has one live successful
// validation of this mutation service. That fingerprint records evidence only. The service may publish
// once its ProcessEvent signature, ABI/reflection, dependencies, and Game-thread gates validate. A
// Pawn-vtable fallback is prohibited. The Host supplies
// bSweep=false and bTeleport=true; it does not expose UE object pointers or an FHitResult ABI to
// plugins. world and player must come from current snapshots, and stale handles are rejected
// rather than resolving to a later object identity.
typedef struct AnomalyNtePlayerTeleportRequestV1 {
    uint32_t struct_size; uint32_t flags;
    AnomalyGenerationHandleV1 world; AnomalyGenerationHandleV1 player;
    double position[3];
} AnomalyNtePlayerTeleportRequestV1;
typedef struct AnomalyNtePlayerTeleportServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    // Valid only from the Game callback domain. It returns FAILED if UE rejects the request or
    // the post-call location check does not reach the requested position.
    AnomalyStatusV1 (ANOMALY_CALL *teleport)(void* user,
        const AnomalyNtePlayerTeleportRequestV1* request);
} AnomalyNtePlayerTeleportServiceV1;

typedef enum AnomalyNteEntityFlagsV1 {
    ANOMALY_NTE_ENTITY_V1_NONE = 0,
    ANOMALY_NTE_ENTITY_V1_STATIC = 1u << 0u,
    ANOMALY_NTE_ENTITY_V1_STATIONARY = 1u << 1u,
    ANOMALY_NTE_ENTITY_V1_MOVABLE = 1u << 2u,
    ANOMALY_NTE_ENTITY_V1_LOCAL_PLAYER = 1u << 3u
} AnomalyNteEntityFlagsV1;
typedef struct AnomalyNteEntityFrameV1 {
    uint32_t struct_size; uint32_t flags; uint64_t generation; uint64_t sequence;
    uint32_t entity_count; uint32_t reserved;
    double camera_position[3]; double camera_rotation[3];
    float horizontal_fov_degrees; uint32_t reserved2;
} AnomalyNteEntityFrameV1;
typedef struct AnomalyNteEntitySnapshotV1 {
    uint32_t struct_size; uint32_t flags; AnomalyGenerationHandleV1 handle;
    uint64_t entity_id; uint64_t class_id;
    uint32_t entity_name_id; uint32_t class_name_id;
    double bounds_center[3]; double bounds_extent[3];
} AnomalyNteEntitySnapshotV1;
// A zero filter ID is a wildcard. required/excluded flags are evaluated against
// AnomalyNteEntityFlagsV1. flags is reserved and must be zero. generation zero selects the
// current cached frame; later pages must pass the returned generation to prevent accidental
// cross-frame iteration. A stale non-zero generation returns NOT_FOUND and no cached frame
// returns UNAVAILABLE. capacity must not exceed ANOMALY_NTE_ENTITY_PAGE_V1_MAX_CAPACITY. On
// INVALID_ARGUMENT, NOT_FOUND, or UNAVAILABLE, the Host leaves destination and result untouched;
// every destination slot through capacity must advertise the full
// AnomalyNteEntitySnapshotV1::struct_size before the call. On success with non-zero capacity,
// next_offset equals offset + returned. When offset exceeds total_matches, the Host returns a
// successful terminal empty page and clamps next_offset to total_matches.
typedef struct AnomalyNteEntityPageRequestV1 {
    uint32_t struct_size; uint32_t flags;
    uint64_t generation;
    uint32_t offset; uint32_t capacity;
    uint64_t class_id;
    uint32_t class_name_id; uint32_t entity_name_id;
    uint32_t required_flags; uint32_t excluded_flags;
} AnomalyNteEntityPageRequestV1;
typedef struct AnomalyNteEntityPageResultV1 {
    uint32_t struct_size; uint32_t flags;
    uint64_t generation; uint64_t sequence;
    uint32_t total_matches; uint32_t returned;
    uint32_t next_offset; uint32_t reserved;
} AnomalyNteEntityPageResultV1;
typedef struct AnomalyNteEntityComponentBoundsV1 {
    uint32_t struct_size; uint32_t flags;
    AnomalyGenerationHandleV1 entity; uint64_t sequence;
    double bounds_center[3]; double bounds_extent[3];
} AnomalyNteEntityComponentBoundsV1;
typedef struct AnomalyNteEntityBoolPropertyV1 {
    uint32_t struct_size; uint32_t flags;
    AnomalyGenerationHandleV1 entity; uint64_t sequence;
    uint32_t value; uint32_t reserved;
} AnomalyNteEntityBoolPropertyV1;
typedef struct AnomalyNteEntitiesServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *frame)(void* user, AnomalyNteEntityFrameV1* frame);
    AnomalyStatusV1 (ANOMALY_CALL *snapshot_at)(void* user, uint64_t generation,
        uint32_t index, AnomalyNteEntitySnapshotV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *class_name_utf8)(void* user, uint64_t class_id,
        char* destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *entity_name_utf8)(void* user, uint64_t entity_id,
        char* destination, size_t* inout_size);
    // Serves only the Host-cached frame. capacity is capped by the Host read budget and every
    // destination element must advertise AnomalyNteEntitySnapshotV1::struct_size.
    AnomalyStatusV1 (ANOMALY_CALL *page)(void* user,
        const AnomalyNteEntityPageRequestV1* request,
        AnomalyNteEntitySnapshotV1* destination,
        AnomalyNteEntityPageResultV1* result);
    // These bounded reflected reads are valid only from the Host's Game callback domain.
    AnomalyStatusV1 (ANOMALY_CALL *component_bounds)(void* user,
        AnomalyGenerationHandleV1 entity, AnomalyStringViewV1 property_name,
        AnomalyNteEntityComponentBoundsV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *bool_property)(void* user,
        AnomalyGenerationHandleV1 entity, AnomalyStringViewV1 property_name,
        AnomalyNteEntityBoolPropertyV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *fname_property_utf8)(void* user,
        AnomalyGenerationHandleV1 entity, AnomalyStringViewV1 property_name,
        char* destination, size_t* inout_size);
} AnomalyNteEntitiesServiceV1;

// Actor discovery is intentionally separate from the high-frequency Entity snapshot. The first
// frame request in a World scans every loaded UWorld level and caches the result for that World.
// Reflected reads are valid only from the Host's Game callback domain.
typedef struct AnomalyNteActorsServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *frame)(void* user, AnomalyNteEntityFrameV1* frame);
    AnomalyStatusV1 (ANOMALY_CALL *snapshot_at)(void* user, uint64_t generation,
        uint32_t index, AnomalyNteEntitySnapshotV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *class_name_utf8)(void* user, uint64_t class_id,
        char* destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *entity_name_utf8)(void* user, uint64_t entity_id,
        char* destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *page)(void* user,
        const AnomalyNteEntityPageRequestV1* request,
        AnomalyNteEntitySnapshotV1* destination,
        AnomalyNteEntityPageResultV1* result);
    AnomalyStatusV1 (ANOMALY_CALL *component_bounds)(void* user,
        AnomalyGenerationHandleV1 actor, AnomalyStringViewV1 property_name,
        AnomalyNteEntityComponentBoundsV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *bool_property)(void* user,
        AnomalyGenerationHandleV1 actor, AnomalyStringViewV1 property_name,
        AnomalyNteEntityBoolPropertyV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *fname_property_utf8)(void* user,
        AnomalyGenerationHandleV1 actor, AnomalyStringViewV1 property_name,
        char* destination, size_t* inout_size);
} AnomalyNteActorsServiceV1;

// Sampling metrics describe Host work, not a per-plugin traversal. The active Profile's
// feature matrix remains available through AnomalyNteBuildServiceV1::feature_state. A page
// cache hit records service from the current immutable Entity-frame cache, not a separately
// memoized page-result lookup.
typedef uint32_t AnomalyNteMetricsFlagsV1;
#define ANOMALY_NTE_METRICS_V1_VALID (1u << 0u)
typedef struct AnomalyNteSnapshotMetricsV1 {
    uint32_t struct_size; uint32_t flags;
    uint64_t tick_sequence; uint64_t session_event_sequence;
    uint64_t snapshot_tick_count; uint64_t latest_snapshot_cost_micros;
    uint64_t total_snapshot_cost_micros; uint64_t max_snapshot_cost_micros;
    uint64_t player_refresh_count; uint64_t player_cache_hit_count;
    uint64_t entity_refresh_count; uint64_t entity_cache_hit_count;
    uint64_t entity_page_request_count; uint64_t entity_page_cache_hit_count;
} AnomalyNteSnapshotMetricsV1;
typedef struct AnomalyNteMetricsServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *snapshot)(void* user,
        AnomalyNteSnapshotMetricsV1* metrics);
} AnomalyNteMetricsServiceV1;
#ifdef __cplusplus
}
#endif
