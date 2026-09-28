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
#define ANOMALY_NTE_MAP_LANDMARKS_SERVICE_V1_ID "anomaly.nte.map-landmarks"
#define ANOMALY_NTE_MAP_LANDMARKS_SERVICE_V1_VERSION 1u
#define ANOMALY_NTE_MAP_LANDMARK_V1_ID_MAX_BYTES 128u
#define ANOMALY_NTE_MAP_LANDMARK_V1_WORLD_MAX_UTF8_BYTES 2048u
#define ANOMALY_NTE_NAVIGATION_SERVICE_V1_ID "anomaly.nte.navigation"
#define ANOMALY_NTE_NAVIGATION_SERVICE_V1_VERSION 1u
#define ANOMALY_NTE_ENTITIES_SERVICE_V1_ID "anomaly.nte.entities"
#define ANOMALY_NTE_ENTITIES_SERVICE_V1_VERSION 1u
#define ANOMALY_NTE_ACTORS_SERVICE_V1_ID "anomaly.nte.actors"
#define ANOMALY_NTE_ACTORS_SERVICE_V1_VERSION 1u
#define ANOMALY_NTE_COMBAT_SERVICE_V1_ID "anomaly.nte.combat"
#define ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION 1u
#define ANOMALY_NTE_SKILLS_SERVICE_V1_ID "anomaly.nte.skills"
#define ANOMALY_NTE_SKILLS_SERVICE_V1_VERSION 1u
#define ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_ID "anomaly.nte.skill-invocation"
#define ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_VERSION 1u
#define ANOMALY_NTE_ENTITY_PAGE_V1_MAX_CAPACITY 256u
#define ANOMALY_NTE_SKILL_PAGE_V1_MAX_CAPACITY 128u
#define ANOMALY_NTE_METRICS_SERVICE_V1_ID "anomaly.nte.metrics"
#define ANOMALY_NTE_METRICS_SERVICE_V1_VERSION 1u
#define ANOMALY_NTE_ESC_MENU_BUTTON_SERVICE_V1_ID "anomaly.nte.esc-menu-button"
#define ANOMALY_NTE_ESC_MENU_BUTTON_SERVICE_V1_VERSION 1u
#define ANOMALY_NTE_PICKUP_SERVICE_V1_ID "anomaly.nte.pickup"
#define ANOMALY_NTE_PICKUP_SERVICE_V1_VERSION 1u
#define ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_ID "anomaly.nte.ui-buttons"
#define ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_VERSION 1u
#define ANOMALY_NTE_UI_BUTTON_V1_NAME_MAX_BYTES 128u
#define ANOMALY_NTE_UI_BUTTON_V1_TEXT_MAX_BYTES 256u
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
// Hold state reported by AnomalyNtePlayerServiceV1::hold_snapshot.
// HELD is set while the Host is holding the local character. REFUSED is set when the most recent
// engage request could not take; an idle snapshot reports no live values, because resolving the
// movement component is a long reflection walk that a per-frame reader must not pay for.
#define ANOMALY_NTE_PLAYER_HOLD_V1_HELD (1u << 0u)
#define ANOMALY_NTE_PLAYER_HOLD_V1_REFUSED (1u << 1u)
typedef struct AnomalyNtePlayerHoldSnapshotV1 {
    uint32_t struct_size; uint32_t flags; double gravity_scale; double velocity[3];
    // EMovementMode value read back from the movement component while HELD. The hold is only
    // effective while the character is out of MOVE_Falling (3), so this is the value that shows
    // whether the game overwrote the hold's own write.
    uint32_t movement_mode; uint32_t reserved;
} AnomalyNtePlayerHoldSnapshotV1;

typedef struct AnomalyNtePlayerServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *snapshot)(void* user, AnomalyNtePlayerSnapshotV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *esp_snapshot)(void* user, AnomalyNtePlayerEspSnapshotV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *camera_snapshot)(void* user,
        AnomalyNteCameraSnapshotV1* snapshot);
    // Present only when struct_size covers them; callers must check struct_size before use.
    // A plugin must declare the explicit nte-player-hold capability for these three. They stop
    // the local character without moving it: engaging zeroes the movement component's gravity
    // scale, clears velocity and takes the character out of its falling state; releasing clears
    // velocity again and restores the original gravity scale in a grounded mode, so the engine
    // re-checks the floor where the character actually is instead of settling a fall that began
    // somewhere else. engage is idempotent and releasing without an engage is not an error. A
    // caller inside the Game callback domain gets the outcome directly, any other thread's
    // request is queued for the Game tick and its outcome appears in the next snapshot. The Host
    // resolves the local pawn itself and never exposes UE object pointers.
    AnomalyStatusV1 (ANOMALY_CALL *hold_engage)(void* user);
    AnomalyStatusV1 (ANOMALY_CALL *hold_release)(void* user);
    AnomalyStatusV1 (ANOMALY_CALL *hold_snapshot)(void* user,
        AnomalyNtePlayerHoldSnapshotV1* snapshot);
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
//
// By default (flags 0) the Host streams the destination in before moving the player, because moving
// into terrain that has not loaded drops the player through the world. That completes on a later
// Game tick: the call reports OK once the preload was accepted, and the post-call location check
// happens on the completing tick instead of before this call returns. The Host also holds the
// character for the whole window and hands it back in a grounded mode at the destination (the same
// writes anomaly.nte.player-hold exposes), so the arrival is not settled as a single fall spanning
// the height difference between the two places. A Host that cannot preload, or that cannot
// establish the hold, degrades to the immediate behaviour. Callers that need the previous
// synchronous semantics pass ANOMALY_NTE_PLAYER_TELEPORT_REQUEST_V1_IMMEDIATE.
#define ANOMALY_NTE_PLAYER_TELEPORT_REQUEST_V1_IMMEDIATE 1u
typedef struct AnomalyNtePlayerTeleportRequestV1 {
    uint32_t struct_size; uint32_t flags;
    AnomalyGenerationHandleV1 world; AnomalyGenerationHandleV1 player;
    double position[3];
} AnomalyNtePlayerTeleportRequestV1;
typedef struct AnomalyNtePlayerTeleportPreloadRequestV1 {
    uint32_t struct_size; uint32_t flags;
    double position[3];
    // See ANOMALY_UE5_STREAMING_SOURCE_DURATION_V1_UNTIL_CLEARED: zero keeps the
    // preload window open until cancel_preload, any other value expires it that many
    // milliseconds after the call.
    uint32_t duration_milliseconds; uint32_t reserved;
} AnomalyNtePlayerTeleportPreloadRequestV1;
typedef struct AnomalyNtePlayerTeleportServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    // Valid only from the Game callback domain. It returns FAILED if UE rejects the request or
    // the post-call location check does not reach the requested position.
    AnomalyStatusV1 (ANOMALY_CALL *teleport)(void* user,
        const AnomalyNtePlayerTeleportRequestV1* request);
    // Present only when struct_size covers them; callers must check struct_size before use.
    // preload asks the Host to stream the destination in before the teleport instead of
    // teleporting into unloaded terrain, and does not move anything itself. The Host resolves
    // the local player controller and validates the position, so a failure degrades to a plain
    // teleport. cancel_preload ends the window early and is idempotent.
    AnomalyStatusV1 (ANOMALY_CALL *preload)(void* user,
        const AnomalyNtePlayerTeleportPreloadRequestV1* request);
    AnomalyStatusV1 (ANOMALY_CALL *cancel_preload)(void* user);
} AnomalyNtePlayerTeleportServiceV1;

// Holds the local character still by writing its movement component, so a window that unloads
// the cells under the player cannot drop them through the world. A plugin must declare the
// explicit nte-player-hold capability. The Host resolves the local pawn through the same
// reflection chain it uses elsewhere and never exposes UE object pointers: engaging zeroes the
// gravity scale, clears velocity and takes the character out of its falling state, releasing
// clears velocity again and restores the original gravity scale in a grounded mode. engage is
// idempotent and releasing without an engage is not an error. snapshots report the hold state
// together with the live gravity scale, velocity and movement mode.
#define ANOMALY_NTE_PLAYER_HOLD_SERVICE_V1_ID "anomaly.nte.player-hold"
#define ANOMALY_NTE_PLAYER_HOLD_SERVICE_V1_VERSION 1u
typedef struct AnomalyNtePlayerHoldServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *engage)(void* user);
    AnomalyStatusV1 (ANOMALY_CALL *release)(void* user);
    AnomalyStatusV1 (ANOMALY_CALL *snapshot)(void* user,
        AnomalyNtePlayerHoldSnapshotV1* snapshot);
} AnomalyNtePlayerHoldServiceV1;

// Enumerates the active map's transferable landmarks and executes the game's map-icon transfer
// bridge. A plugin must declare the explicit nte-map-landmarks capability. Landmark snapshots
// are immutable for their sequence; a request with an obsolete sequence or index is rejected.
typedef uint32_t AnomalyNteMapLandmarkFlagsV1;
#define ANOMALY_NTE_MAP_LANDMARK_V1_VALID (1u << 0u)
#define ANOMALY_NTE_MAP_LANDMARK_V1_DESTINATION_OVERRIDDEN (1u << 1u)
typedef enum AnomalyNteMapLandmarkTransferModeV1 {
    ANOMALY_NTE_MAP_LANDMARK_TRANSFER_V1_NORMAL = 0,
    ANOMALY_NTE_MAP_LANDMARK_TRANSFER_V1_SELLING_INDULGENCES = 1
} AnomalyNteMapLandmarkTransferModeV1;
typedef struct AnomalyNteMapLandmarkSnapshotV1 {
    uint32_t struct_size; uint32_t flags; uint64_t sequence;
    uint32_t point_type; int32_t floor;
    double world_position[3]; double destination[3];
    char teleport_id[ANOMALY_NTE_MAP_LANDMARK_V1_ID_MAX_BYTES + 1u];
    char world[ANOMALY_NTE_MAP_LANDMARK_V1_WORLD_MAX_UTF8_BYTES + 1u];
} AnomalyNteMapLandmarkSnapshotV1;
typedef struct AnomalyNteMapLandmarkTeleportRequestV1 {
    uint32_t struct_size; uint32_t mode;
    uint64_t sequence; uint32_t index; uint32_t flags;
} AnomalyNteMapLandmarkTeleportRequestV1;
typedef struct AnomalyNteMapLandmarksServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    uint64_t (ANOMALY_CALL *sequence)(void* user);
    uint32_t (ANOMALY_CALL *count)(void* user);
    AnomalyStatusV1 (ANOMALY_CALL *snapshot_at)(
        void* user, uint32_t index, AnomalyNteMapLandmarkSnapshotV1* snapshot);
    // Valid only from the Game callback domain. The Host resolves the requested item from the
    // current immutable catalog; no UE object pointer or raw map identifier crosses the ABI.
    AnomalyStatusV1 (ANOMALY_CALL *teleport)(
        void* user, const AnomalyNteMapLandmarkTeleportRequestV1* request);
} AnomalyNteMapLandmarksServiceV1;

// Native NTE navigation is exposed through the Host's verified ProcessEvent
// bridge. Calls are valid only from the Game thread and operate on the current
// local player controller; no UE object pointers cross the ABI boundary.
typedef struct AnomalyNteNavigationServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *move_to_location)(
        void* user, const double destination[3]);
    AnomalyStatusV1 (ANOMALY_CALL *stop_movement)(void* user);
} AnomalyNteNavigationServiceV1;

// Nearby pickup is a Host-owned interaction bridge. The request is accepted only from the
// active Game callback domain; it never exposes UE object pointers, reflected functions, or
// Profile offsets. Confirmation is reported independently from the interaction trigger.
typedef struct AnomalyNtePickupRequestV1 {
    uint32_t struct_size;
    uint32_t flags;
    double radius;
    uint32_t maximum_items;
    uint32_t reserved;
} AnomalyNtePickupRequestV1;

typedef enum AnomalyNtePickupStateV1 {
    ANOMALY_NTE_PICKUP_V1_IDLE = 0,
    ANOMALY_NTE_PICKUP_V1_QUEUED = 1,
    ANOMALY_NTE_PICKUP_V1_CHECKING = 2,
    ANOMALY_NTE_PICKUP_V1_COMPLETE = 3
} AnomalyNtePickupStateV1;

typedef uint32_t AnomalyNtePickupFlagsV1;
#define ANOMALY_NTE_PICKUP_V1_NONE 0u
#define ANOMALY_NTE_PICKUP_V1_VALID (1u << 0u)
#define ANOMALY_NTE_PICKUP_V1_CHECKING_FLAG (1u << 1u)
#define ANOMALY_NTE_PICKUP_V1_HAS_UNCONFIRMED (1u << 2u)

typedef struct AnomalyNtePickupSnapshotV1 {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t sequence;
    uint32_t state;
    uint32_t status;
    uint32_t nearby;
    uint32_t triggered;
    uint32_t confirmed;
    uint32_t checking;
    uint32_t unconfirmed;
    uint32_t skipped;
} AnomalyNtePickupSnapshotV1;

typedef struct AnomalyNtePickupServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *request_nearby)(
        void* user, const AnomalyNtePickupRequestV1* request);
    AnomalyStatusV1 (ANOMALY_CALL *snapshot)(
        void* user, AnomalyNtePickupSnapshotV1* snapshot);
} AnomalyNtePickupServiceV1;

// In-game UI buttons (UMG Button, CommonUI CommonButtonBase, HTUI_Button, radio boxes and
// check boxes used as tabs, and HTUI_ListItem list entries). A plugin must
// declare the explicit nte-ui-buttons capability. The Host scans the widget tree, classifies
// every button as clickable, blocked or hidden, and clicks by invoking the button's own
// press -> release -> click handlers; it never synthesizes mouse or keyboard input and never
// exposes UE object pointers.
//
// Work is asynchronous. request_scan, request_pick and request_click only queue a request and
// return a request handle; the Host runs it in bounded slices on later Game ticks. A request
// moves QUEUED -> RUNNING -> COMPLETE (a click completes on the tick it runs), and its final
// outcome is its status: OK, or CANCELLED, NOT_FOUND (stale handle), CONFLICT (not clickable),
// UNAVAILABLE or FAILED. Every entry point may be called from any thread.
//
// A completed scan publishes an immutable catalog identified by catalog_sequence. Catalog
// reads name the sequence they were enumerated from and fail with NOT_FOUND once a newer
// catalog replaces it. Button handles stay valid until the object registry changes; the Host
// re-validates the object and re-evaluates clickability, including whether another window now
// covers the button, immediately before it clicks.
typedef enum AnomalyNteUiButtonKindV1 {
    ANOMALY_NTE_UI_BUTTON_KIND_V1_UMG = 1,
    ANOMALY_NTE_UI_BUTTON_KIND_V1_COMMON = 2,
    ANOMALY_NTE_UI_BUTTON_KIND_V1_HTUI = 3,
    // HTUI_RadioBox, a bare HTRadioBox or a UMG CheckBox (tab pages). Clicking selects it; an
    // already selected one stays clickable and the click changes nothing.
    ANOMALY_NTE_UI_BUTTON_KIND_V1_RADIO = 4,
    // HTUI_ListItem list entry; the click reaches the owning list's item-click handler.
    ANOMALY_NTE_UI_BUTTON_KIND_V1_LIST_ENTRY = 5
} AnomalyNteUiButtonKindV1;

typedef enum AnomalyNteUiButtonCategoryV1 {
    ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_CLICKABLE = 1,
    ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_BLOCKED = 2,
    ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_HIDDEN = 3
} AnomalyNteUiButtonCategoryV1;

// Why a button is not clickable. Visibility reasons place it in HIDDEN; any other reason in
// BLOCKED.
typedef uint32_t AnomalyNteUiButtonReasonsV1;
#define ANOMALY_NTE_UI_BUTTON_REASON_V1_NONE 0u
#define ANOMALY_NTE_UI_BUTTON_REASON_V1_COLLAPSED_SELF (1u << 0u)
#define ANOMALY_NTE_UI_BUTTON_REASON_V1_COLLAPSED_ANCESTOR (1u << 1u)
#define ANOMALY_NTE_UI_BUTTON_REASON_V1_NOT_IN_VIEWPORT (1u << 2u)
#define ANOMALY_NTE_UI_BUTTON_REASON_V1_DETACHED (1u << 3u)
#define ANOMALY_NTE_UI_BUTTON_REASON_V1_DISABLED (1u << 4u)
#define ANOMALY_NTE_UI_BUTTON_REASON_V1_NOT_INTERACTABLE (1u << 5u)
#define ANOMALY_NTE_UI_BUTTON_REASON_V1_LOCKED (1u << 6u)
#define ANOMALY_NTE_UI_BUTTON_REASON_V1_QUERY_FAILED (1u << 7u)
#define ANOMALY_NTE_UI_BUTTON_REASON_V1_INACTIVE_PAGE (1u << 8u)
#define ANOMALY_NTE_UI_BUTTON_REASON_V1_TRANSPARENT (1u << 9u)
#define ANOMALY_NTE_UI_BUTTON_REASON_V1_NOT_HIT_TESTABLE (1u << 10u)
#define ANOMALY_NTE_UI_BUTTON_REASON_V1_CLOSING (1u << 11u)
#define ANOMALY_NTE_UI_BUTTON_REASON_V1_OCCLUDED (1u << 12u)

// window, owner and root are UserWidget object names: the window shown by the UI layer that
// holds the button, the UserWidget whose widget tree declares it, and the outermost layout.
// path lists every UserWidget from the outermost to the innermost, separated by " / ".
// depth counts the widgets above the button; a pick lists deeper (inner) buttons first.
// cause names the widget or window behind the first hiding or blocking reason. Strings are
// null-terminated UTF-8, truncated on a code point boundary.
typedef struct AnomalyNteUiButtonSnapshotV1 {
    uint32_t struct_size; uint32_t kind;
    uint64_t catalog_sequence;
    AnomalyGenerationHandleV1 button;
    uint32_t index; uint32_t category;
    uint32_t reasons; uint32_t depth;
    char name[ANOMALY_NTE_UI_BUTTON_V1_NAME_MAX_BYTES + 1u];
    char class_name[ANOMALY_NTE_UI_BUTTON_V1_NAME_MAX_BYTES + 1u];
    char window[ANOMALY_NTE_UI_BUTTON_V1_NAME_MAX_BYTES + 1u];
    char owner[ANOMALY_NTE_UI_BUTTON_V1_NAME_MAX_BYTES + 1u];
    char root[ANOMALY_NTE_UI_BUTTON_V1_NAME_MAX_BYTES + 1u];
    char cause[ANOMALY_NTE_UI_BUTTON_V1_TEXT_MAX_BYTES + 1u];
    char text[ANOMALY_NTE_UI_BUTTON_V1_TEXT_MAX_BYTES + 1u];
    char path[ANOMALY_NTE_UI_BUTTON_V1_TEXT_MAX_BYTES + 1u];
} AnomalyNteUiButtonSnapshotV1;

// A UI layer (CommonUI activatable-widget container) and the window it currently shows.
typedef uint32_t AnomalyNteUiWindowFlagsV1;
#define ANOMALY_NTE_UI_WINDOW_V1_ACTIVE (1u << 0u)
#define ANOMALY_NTE_UI_WINDOW_V1_VISIBLE (1u << 1u)
#define ANOMALY_NTE_UI_WINDOW_V1_CLOSING (1u << 2u)
#define ANOMALY_NTE_UI_WINDOW_V1_MODAL (1u << 3u)
#define ANOMALY_NTE_UI_WINDOW_V1_HIDES_MAIN_FORM (1u << 4u)
#define ANOMALY_NTE_UI_WINDOW_V1_PAUSES_GAME (1u << 5u)
#define ANOMALY_NTE_UI_WINDOW_V1_MENU_INPUT (1u << 6u)
// Shown and covers the input of every window drawn beneath it.
#define ANOMALY_NTE_UI_WINDOW_V1_BLOCKING (1u << 7u)

typedef struct AnomalyNteUiWindowSnapshotV1 {
    uint32_t struct_size; uint32_t flags;
    uint64_t catalog_sequence;
    uint32_t index; uint32_t reserved;
    char layer[ANOMALY_NTE_UI_BUTTON_V1_NAME_MAX_BYTES + 1u];
    char window[ANOMALY_NTE_UI_BUTTON_V1_NAME_MAX_BYTES + 1u];
} AnomalyNteUiWindowSnapshotV1;

typedef uint32_t AnomalyNteUiButtonsStatusFlagsV1;
// The active Profile, the reflected UI types and ProcessEvent are all usable.
#define ANOMALY_NTE_UI_BUTTONS_STATUS_V1_READY (1u << 0u)
#define ANOMALY_NTE_UI_BUTTONS_STATUS_V1_CATALOG (1u << 1u)
#define ANOMALY_NTE_UI_BUTTONS_STATUS_V1_SCANNING (1u << 2u)
// The catalog hit a scan bound and may omit buttons.
#define ANOMALY_NTE_UI_BUTTONS_STATUS_V1_TRUNCATED (1u << 3u)
#define ANOMALY_NTE_UI_BUTTONS_STATUS_V1_PICK_AVAILABLE (1u << 4u)

typedef struct AnomalyNteUiButtonsStatusV1 {
    uint32_t struct_size; uint32_t flags;
    uint64_t catalog_sequence;
    uint32_t button_count; uint32_t window_count;
    uint32_t clickable_count; uint32_t blocked_count;
    uint32_t hidden_count; uint32_t open_requests;
    uint32_t objects_scanned; uint32_t process_event_calls;
    uint32_t scan_ticks; uint32_t scan_milliseconds;
} AnomalyNteUiButtonsStatusV1;

// Selects buttons from a catalog. Empty strings match anything. name and text match
// exactly; window matches the button's window, owner, root or any UserWidget on its path.
// category_mask is a set of ANOMALY_NTE_UI_BUTTON_QUERY_V1_CATEGORY bits, 0 for any.
// catalog_sequence 0 selects the current catalog.
#define ANOMALY_NTE_UI_BUTTON_QUERY_V1_CATEGORY(category) (1u << (uint32_t)(category))
typedef struct AnomalyNteUiButtonQueryV1 {
    uint32_t struct_size; uint32_t category_mask;
    uint64_t catalog_sequence;
    AnomalyStringViewV1 name;
    AnomalyStringViewV1 window;
    AnomalyStringViewV1 text;
} AnomalyNteUiButtonQueryV1;

// FORCE clicks even when the button is not clickable. The Host still refuses a stale handle.
#define ANOMALY_NTE_UI_BUTTON_CLICK_V1_FORCE (1u << 0u)
typedef struct AnomalyNteUiButtonClickRequestV1 {
    uint32_t struct_size; uint32_t flags;
    AnomalyGenerationHandleV1 button;
} AnomalyNteUiButtonClickRequestV1;

typedef enum AnomalyNteUiButtonRequestKindV1 {
    ANOMALY_NTE_UI_BUTTON_REQUEST_V1_SCAN = 1,
    ANOMALY_NTE_UI_BUTTON_REQUEST_V1_PICK = 2,
    ANOMALY_NTE_UI_BUTTON_REQUEST_V1_CLICK = 3
} AnomalyNteUiButtonRequestKindV1;

typedef enum AnomalyNteUiButtonRequestStateV1 {
    ANOMALY_NTE_UI_BUTTON_REQUEST_V1_QUEUED = 1,
    ANOMALY_NTE_UI_BUTTON_REQUEST_V1_RUNNING = 2,
    ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE = 3
} AnomalyNteUiButtonRequestStateV1;

typedef uint32_t AnomalyNteUiButtonOutcomeFlagsV1;
#define ANOMALY_NTE_UI_BUTTON_OUTCOME_V1_FORCED (1u << 0u)
// HTUI_Button only: the press armed the button and the click passed its own gate. An HTUI
// click that does not pass completes with FAILED because the game discarded it.
#define ANOMALY_NTE_UI_BUTTON_OUTCOME_V1_PRESS_ARMED (1u << 1u)
#define ANOMALY_NTE_UI_BUTTON_OUTCOME_V1_CLICK_ACCEPTED (1u << 2u)
// Scan and pick: the catalog they used hit a scan bound.
#define ANOMALY_NTE_UI_BUTTON_OUTCOME_V1_TRUNCATED (1u << 3u)

// catalog_sequence is the catalog a scan published, or the catalog a pick or click used.
// reasons is set when a click is refused as not clickable. hit_count is the number of
// hovered buttons a pick found, innermost first; read them with pick_hit_at.
typedef struct AnomalyNteUiButtonRequestSnapshotV1 {
    uint32_t struct_size; uint32_t kind;
    AnomalyGenerationHandleV1 request;
    uint32_t state; uint32_t status;
    uint64_t catalog_sequence;
    uint32_t reasons; uint32_t outcome;
    uint32_t invocations; uint32_t hit_count;
    uint32_t checked; uint32_t process_event_calls;
    char detail[ANOMALY_NTE_UI_BUTTON_V1_TEXT_MAX_BYTES + 1u];
} AnomalyNteUiButtonRequestSnapshotV1;

typedef struct AnomalyNteUiButtonsServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *status)(void* user, AnomalyNteUiButtonsStatusV1* status);
    AnomalyStatusV1 (ANOMALY_CALL *button_at)(
        void* user, uint64_t catalog_sequence, uint32_t index,
        AnomalyNteUiButtonSnapshotV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *window_at)(
        void* user, uint64_t catalog_sequence, uint32_t index,
        AnomalyNteUiWindowSnapshotV1* snapshot);
    // Copies the first match (catalog order: clickable, blocked, hidden) and the match count.
    // Returns NOT_FOUND with a zero count when nothing matches.
    AnomalyStatusV1 (ANOMALY_CALL *find)(
        void* user, const AnomalyNteUiButtonQueryV1* query,
        AnomalyNteUiButtonSnapshotV1* first, uint32_t* match_count);
    AnomalyStatusV1 (ANOMALY_CALL *request_scan)(void* user, AnomalyGenerationHandleV1* request);
    // Rescans, then reports the buttons Slate marks as hovered by the real cursor.
    AnomalyStatusV1 (ANOMALY_CALL *request_pick)(void* user, AnomalyGenerationHandleV1* request);
    AnomalyStatusV1 (ANOMALY_CALL *request_click)(
        void* user, const AnomalyNteUiButtonClickRequestV1* click,
        AnomalyGenerationHandleV1* request);
    AnomalyStatusV1 (ANOMALY_CALL *request_snapshot)(
        void* user, AnomalyGenerationHandleV1 request,
        AnomalyNteUiButtonRequestSnapshotV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *pick_hit_at)(
        void* user, AnomalyGenerationHandleV1 request, uint32_t index,
        AnomalyNteUiButtonSnapshotV1* snapshot);
    // Completes a QUEUED or RUNNING request with CANCELLED; CONFLICT once it is complete.
    AnomalyStatusV1 (ANOMALY_CALL *cancel)(void* user, AnomalyGenerationHandleV1 request);
} AnomalyNteUiButtonsServiceV1;

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
// frame request in a World scans every loaded UWorld level; the result is then re-scanned on the
// Host's actor sampling interval (and immediately whenever the World changes), so entries for
// destroyed actors do not linger and newly spawned actors become visible.
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

// Combat snapshots are Host-cached. CHARACTER_EVENT damage is captured from
// AHTAbilityCharacter::CharacterOnDamaged with participants and DamageGEDef.
typedef uint32_t AnomalyNteCombatantFlagsV1;
#define ANOMALY_NTE_COMBATANT_V1_DEAD (1u << 0u)
#define ANOMALY_NTE_COMBATANT_V1_VALID ANOMALY_NTE_SNAPSHOT_V1_VALID
#define ANOMALY_NTE_COMBATANT_V1_STALE ANOMALY_NTE_SNAPSHOT_V1_STALE
#define ANOMALY_NTE_COMBATANT_V1_PARTIAL ANOMALY_NTE_SNAPSHOT_V1_PARTIAL
typedef struct AnomalyNteCombatantSnapshotV1 {
    uint32_t struct_size; uint32_t flags;
    uint64_t sequence;
    AnomalyGenerationHandleV1 world;
    AnomalyGenerationHandleV1 character;
    AnomalyGenerationHandleV1 target;
    double hp; double max_hp; double shield;
} AnomalyNteCombatantSnapshotV1;

typedef uint32_t AnomalyNteDamageFlagsV1;
#define ANOMALY_NTE_DAMAGE_V1_CLIENT_PRESENTED (1u << 0u)
#define ANOMALY_NTE_DAMAGE_V1_CRITICAL (1u << 1u)
#define ANOMALY_NTE_DAMAGE_V1_HEAD_HIT (1u << 2u)
#define ANOMALY_NTE_DAMAGE_V1_WEAK_UNBALANCE (1u << 3u)
// final_damage is the rounded FHTDamageEvent::Damage value. Display-only
// metadata is unavailable unless a separate flag says otherwise.
#define ANOMALY_NTE_DAMAGE_V1_CHARACTER_EVENT (1u << 4u)
// Without CRITICAL_VALID, an unset CRITICAL bit means unknown, not non-critical.
#define ANOMALY_NTE_DAMAGE_V1_CRITICAL_VALID (1u << 5u)
typedef struct AnomalyNteDamageEventV1 {
    uint32_t struct_size; uint32_t flags;
    uint64_t sequence; uint64_t tick_sequence;
    AnomalyGenerationHandleV1 world;
    AnomalyGenerationHandleV1 attacker;
    AnomalyGenerationHandleV1 victim;
    uint64_t source_id;
    int64_t display_damage; int64_t basic_damage; int64_t final_damage;
    double hit_location[3];
    uint32_t damage_type; uint32_t display_type;
    uint32_t reaction_type; uint32_t reaction_display_type;
} AnomalyNteDamageEventV1;

// Unified low-latency combat stream. Damage entries are emitted from the
// CharacterOnDamaged hook and enriched by FHTDamageTextInfo when available.
// Heal and buff entries come from the cached combat/ASC snapshots; UI callbacks
// are optional enrichers. name_id identifies a FName or generation-local source.
typedef enum AnomalyNteCombatEventKindV1 {
    ANOMALY_NTE_COMBAT_EVENT_V1_DAMAGE = 1,
    ANOMALY_NTE_COMBAT_EVENT_V1_HEAL = 2,
    ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_ADD = 3,
    ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_REMOVE = 4
} AnomalyNteCombatEventKindV1;
typedef uint32_t AnomalyNteCombatEventFlagsV1;
#define ANOMALY_NTE_COMBAT_EVENT_V1_CRITICAL (1u << 0u)
#define ANOMALY_NTE_COMBAT_EVENT_V1_HEAD_HIT (1u << 1u)
#define ANOMALY_NTE_COMBAT_EVENT_V1_WEAK_UNBALANCE (1u << 2u)
#define ANOMALY_NTE_COMBAT_EVENT_V1_DISPLAY_VALID (1u << 3u)
#define ANOMALY_NTE_COMBAT_EVENT_V1_NAME_VALID (1u << 4u)
#define ANOMALY_NTE_COMBAT_EVENT_V1_PARTIAL (1u << 5u)
#define ANOMALY_NTE_COMBAT_EVENT_V1_CRITICAL_VALID (1u << 6u)
typedef struct AnomalyNteCombatEventV1 {
    uint32_t struct_size; uint32_t kind; uint32_t flags; uint32_t reserved;
    uint64_t sequence; uint64_t tick_sequence;
    AnomalyGenerationHandleV1 world;
    AnomalyGenerationHandleV1 source;
    AnomalyGenerationHandleV1 target;
    uint64_t name_id;
    int64_t value; int64_t basic_value; int64_t final_value;
    float duration_seconds; int32_t stack_count;
    uint32_t damage_type; uint32_t display_type;
    uint32_t reaction_type; uint32_t reaction_display_type;
} AnomalyNteCombatEventV1;

typedef enum AnomalyNteCombatDirectionV1 {
    ANOMALY_NTE_COMBAT_DIRECTION_V1_ANY = 0,
    ANOMALY_NTE_COMBAT_DIRECTION_V1_AS_ATTACKER = 1,
    ANOMALY_NTE_COMBAT_DIRECTION_V1_AS_VICTIM = 2
} AnomalyNteCombatDirectionV1;
typedef struct AnomalyNteCombatStatisticsRequestV1 {
    uint32_t struct_size; uint32_t flags;
    AnomalyGenerationHandleV1 world;
    AnomalyGenerationHandleV1 character;
    uint64_t source_id;
    uint32_t direction; uint32_t reserved;
} AnomalyNteCombatStatisticsRequestV1;
typedef uint32_t AnomalyNteCombatStatisticsFlagsV1;
#define ANOMALY_NTE_COMBAT_STATISTICS_V1_PARTIAL (1u << 0u)
#define ANOMALY_NTE_COMBAT_STATISTICS_V1_OVERFLOW (1u << 1u)
typedef struct AnomalyNteCombatStatisticsV1 {
    uint32_t struct_size; uint32_t flags;
    uint64_t through_sequence;
    uint64_t hit_count; uint64_t critical_count; uint64_t head_hit_count;
    int64_t display_damage_total;
    int64_t basic_damage_total;
    int64_t final_damage_total;
} AnomalyNteCombatStatisticsV1;
typedef struct AnomalyNteCombatServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *current_combatant)(
        void* user, AnomalyNteCombatantSnapshotV1* snapshot);
    uint64_t (ANOMALY_CALL *latest_damage_sequence)(void* user);
    AnomalyStatusV1 (ANOMALY_CALL *next_damage_event)(
        void* user, uint64_t after_sequence, AnomalyNteDamageEventV1* event);
    AnomalyStatusV1 (ANOMALY_CALL *statistics)(
        void* user, const AnomalyNteCombatStatisticsRequestV1* request,
        AnomalyNteCombatStatisticsV1* statistics);
    AnomalyStatusV1 (ANOMALY_CALL *source_name_utf8)(
        void* user, uint64_t source_id, char* destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *participant_path_utf8)(void* user,
        AnomalyGenerationHandleV1 participant, char* destination,
        size_t* inout_size);
    uint64_t (ANOMALY_CALL *latest_event_sequence)(void* user);
    AnomalyStatusV1 (ANOMALY_CALL *next_event)(
        void* user, uint64_t after_sequence, AnomalyNteCombatEventV1* event);
    AnomalyStatusV1 (ANOMALY_CALL *event_name_utf8)(
        void* user, const AnomalyNteCombatEventV1* event,
        char* destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *participant_display_name_utf8)(
        void* user, AnomalyGenerationHandleV1 participant,
        char* destination, size_t* inout_size);
} AnomalyNteCombatServiceV1;

typedef struct AnomalyNteSkillFrameV1 {
    uint32_t struct_size; uint32_t flags;
    uint64_t generation; uint64_t sequence;
    AnomalyGenerationHandleV1 character;
    uint32_t skill_count; uint32_t reserved;
} AnomalyNteSkillFrameV1;
typedef uint32_t AnomalyNteSkillFlagsV1;
#define ANOMALY_NTE_SKILL_V1_ACTIVE (1u << 0u)
#define ANOMALY_NTE_SKILL_V1_INPUT_PRESSED (1u << 1u)
#define ANOMALY_NTE_SKILL_V1_PENDING_REMOVE (1u << 2u)
#define ANOMALY_NTE_SKILL_V1_REMOVE_AFTER_ACTIVATION (1u << 3u)
#define ANOMALY_NTE_SKILL_V1_COOLDOWN_VALID (1u << 4u)
#define ANOMALY_NTE_SKILL_V1_VALID ANOMALY_NTE_SNAPSHOT_V1_VALID
#define ANOMALY_NTE_SKILL_V1_STALE ANOMALY_NTE_SNAPSHOT_V1_STALE
#define ANOMALY_NTE_SKILL_V1_PARTIAL ANOMALY_NTE_SNAPSHOT_V1_PARTIAL
typedef struct AnomalyNteSkillSnapshotV1 {
    uint32_t struct_size; uint32_t flags;
    AnomalyGenerationHandleV1 handle;
    AnomalyGenerationHandleV1 character;
    AnomalyGenerationHandleV1 ability_class;
    uint64_t sequence;
    int32_t level; int32_t input_id;
    float cooldown_remaining_seconds; float cooldown_duration_seconds;
} AnomalyNteSkillSnapshotV1;
typedef struct AnomalyNteSkillPageRequestV1 {
    uint32_t struct_size; uint32_t flags;
    uint64_t generation;
    uint32_t offset; uint32_t capacity;
} AnomalyNteSkillPageRequestV1;
typedef struct AnomalyNteSkillPageResultV1 {
    uint32_t struct_size; uint32_t flags;
    uint64_t generation; uint64_t sequence;
    uint32_t total_skills; uint32_t returned;
    uint32_t next_offset; uint32_t reserved;
} AnomalyNteSkillPageResultV1;
typedef struct AnomalyNteSkillsServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *frame)(void* user, AnomalyNteSkillFrameV1* frame);
    AnomalyStatusV1 (ANOMALY_CALL *snapshot_at)(
        void* user, uint64_t generation, uint32_t index,
        AnomalyNteSkillSnapshotV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *page)(
        void* user, const AnomalyNteSkillPageRequestV1* request,
        AnomalyNteSkillSnapshotV1* destination,
        AnomalyNteSkillPageResultV1* result);
    AnomalyStatusV1 (ANOMALY_CALL *ability_path_utf8)(
        void* user, AnomalyGenerationHandleV1 ability_class,
        char* destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *snapshot_by_handle)(
        void* user, AnomalyGenerationHandleV1 skill,
        AnomalyNteSkillSnapshotV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *ability_display_name_utf8)(
        void* user, AnomalyGenerationHandleV1 ability_class,
        char* destination, size_t* inout_size);
} AnomalyNteSkillsServiceV1;

typedef struct AnomalyNteSkillInvocationRequestV1 {
    uint32_t struct_size; uint32_t flags;
    AnomalyGenerationHandleV1 world;
    AnomalyGenerationHandleV1 character;
    AnomalyGenerationHandleV1 skill;
} AnomalyNteSkillInvocationRequestV1;
typedef struct AnomalyNteSkillInvocationResultV1 {
    uint32_t struct_size; uint32_t flags;
    uint64_t tick_sequence;
    uint32_t accepted; uint32_t reserved;
} AnomalyNteSkillInvocationResultV1;
typedef struct AnomalyNteSkillInvocationServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    // Valid only from the Host Game callback domain. accepted reports the
    // game's bool result; an accepted value of zero is still a successful bridge call.
    AnomalyStatusV1 (ANOMALY_CALL *activate)(
        void* user, const AnomalyNteSkillInvocationRequestV1* request,
        AnomalyNteSkillInvocationResultV1* result);
} AnomalyNteSkillInvocationServiceV1;

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
