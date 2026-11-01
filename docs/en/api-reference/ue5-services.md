# UE5 services

Header: `services/ue5.h`. These are **engine-generic** adaptation services and carry no NTE-specific fields. They are published only after the corresponding symbols of the active [Profile](../user-guide/nte-profiles.md) pass validation; otherwise they stay `UNAVAILABLE` as a Feature. See [API reference overview](README.md) for the shared conventions.

> [!IMPORTANT]
> A service table belongs to one Host lifecycle generation; a cached table from a stopped generation reports `UNAVAILABLE` only (scalar queries return 0).

## `anomaly.ue5.build`

- **ID**: `"anomaly.ue5.build"` · **version** 1 · **capability** `ue5-build`

```c
typedef struct AnomalyUe5BuildServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *build_id)(void* user, char* destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *profile_hash)(void* user, char* destination, size_t* inout_size);
    uint32_t (ANOMALY_CALL *feature_state)(void* user, AnomalyStringViewV1 feature_id);
} AnomalyUe5BuildServiceV1;
```

| Function | Description |
| --- | --- |
| `build_id` | Build identity (two-phase buffer); the current production Runtime has PE fingerprinting disabled, so it returns an empty string |
| `profile_hash` | Hash of the active Profile (two-phase buffer) |
| `feature_state(feature_id)` | Returns `AnomalyFeatureStateV1` (0 = unavailable, 1 = available) |

The host publishes `anomaly.ue5.build` and `anomaly.nte.build` at Adapter startup for Feature discovery. Use `feature_state` to decide whether a capability is available, and never treat the currently empty `build_id` as compatibility evidence.

## `anomaly.ue5.framework`

- **ID**: `"anomaly.ue5.framework"` · **version** 1 · **capability** `game-events`

```c
typedef struct AnomalyUe5FrameworkServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    uint32_t (ANOMALY_CALL *game_thread_id)(void* user);
    uint64_t (ANOMALY_CALL *tick_sequence)(void* user);
    int (ANOMALY_CALL *is_game_thread)(void* user);
} AnomalyUe5FrameworkServiceV1;
```

Provides game-thread anchors: `game_thread_id`, a monotonically increasing `tick_sequence`, and `is_game_thread` (whether the caller is currently on the Game thread).

## `anomaly.ue5.ahud`

- **ID**: `"anomaly.ue5.ahud"` · **version** 1 · **capability** `ue5-ahud`

```c
typedef struct AnomalyUe5AhudFrameV1 {
    uint32_t struct_size; uint32_t flags; void* user;
    uint32_t viewport_width; uint32_t viewport_height;
    int (ANOMALY_CALL *project)(
        void* user, const double world[3], float screen[2], double* depth);
    int (ANOMALY_CALL *measure_text)(
        void* user, AnomalyStringViewV1 text, float scale,
        float* width, float* height);
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
```

After UE's native `ReceiveDrawHUD` completes, the host calls subscribers synchronously on the Game thread; callbacks of the same subscription endpoint run serially and never enter concurrently. `frame`, `frame->user` and every function in them are valid only until the current callback returns; do not cache them and do not call them from other threads. The service does not expose `AHUD*`, `UCanvas*` or any other UE object to plugins. After a successful subscription the host first completes one live UFunction reflection binding; callback admission begins only once the names, owners, parameter types, offsets and `ParmsSize` of all six functions pass validation, and subscribers are not called while the binding is still scanning.

| Function | Description |
| --- | --- |
| `project` | Projects a 3D world position onto the screen; returns non-zero on success and when the position is in front of the camera, `depth` may be null |
| `measure_text` | Measures UTF-8 text using the AHUD default font; returns non-zero on success |
| `draw_text` | Draws UTF-8 text using the AHUD default font; returns non-zero on success |
| `draw_line` | Draws a line segment with the given thickness; returns non-zero on success |
| `draw_rect` | Draws a filled rectangle; returns non-zero on success |
| `subscribe` | Creates an independent synchronous draw subscription and returns a generation handle |
| `unsubscribe` | Blocks future callback admission once it succeeds; when called outside a callback it drains already-entered callbacks before returning |

`color_rgba` uses the byte layout of `ANOMALY_RGBA_V1(red, green, blue, alpha)`. Callbacks should consume only published immutable snapshots and submit draw calls; they should not perform file, network or lifecycle work. Plugin exceptions are isolated by the host boundary; other subscriptions in the same frame still run.

AHUD is the world-space drawing entry point for new plugins; do not run ESP projection or drawing inside `anomaly.ui` Render-thread callbacks.

You may call `unsubscribe` with your own handle from inside a callback. Such a self-unsubscribe only blocks future admission and does not wait for the current callback itself to return; the plugin must keep `callback_user` alive at least until the current callback returns.

## `anomaly.ue5.names`

- **ID**: `"anomaly.ue5.names"` · **version** 1 · **capability** `ue5-names`

```c
typedef struct AnomalyUe5NamesServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *resolve_utf8)(void* user, uint32_t name_id, char* destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *resolve_ftext_utf8)(
        void* user, uintptr_t ftext_address, char* destination, size_t* inout_size);
} AnomalyUe5NamesServiceV1;
```

Decodes an `FName` `name_id` into a UTF-8 string (two-phase buffer).

`resolve_ftext_utf8` accepts an `FText` address that is valid in the current Game snapshot and requires the optional `ue5.ftext` layout validation to pass. It prefers to copy existing localized display text; for text-table references, when no display cache exists yet, it reads the source text of the loaded text table, and that source text is not guaranteed to match the translation for the current language. The operation does not call UE text functions, does not load assets, does not rebuild text history, and does not retain the input address. Unknown types, invalid strings or unloaded text tables return `NOT_FOUND`; plain `FName` resolution remains independently available.

## `anomaly.ue5.objects`

- **ID**: `"anomaly.ue5.objects"` · **version** 1 · **capability** `ue5-objects`

```c
typedef struct AnomalyUe5ObjectSnapshotV1 {
    uint32_t struct_size; uint32_t reserved;
    AnomalyGenerationHandleV1 handle; uint32_t name_id; uint32_t flags;
} AnomalyUe5ObjectSnapshotV1;
typedef struct AnomalyUe5ObjectsServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    uint64_t (ANOMALY_CALL *generation)(void* user);
    uint32_t (ANOMALY_CALL *count)(void* user);
    AnomalyStatusV1 (ANOMALY_CALL *snapshot_at)(void* user, uint32_t index, AnomalyUe5ObjectSnapshotV1*);
    AnomalyStatusV1 (ANOMALY_CALL *snapshot_by_handle)(void* user, AnomalyGenerationHandleV1, AnomalyUe5ObjectSnapshotV1*);
    AnomalyStatusV1 (ANOMALY_CALL *find_exact)(void* user, AnomalyStringViewV1 path, AnomalyGenerationHandleV1*);
} AnomalyUe5ObjectsServiceV1;
```

| Function | Description |
| --- | --- |
| `generation` | Generation of the current object table |
| `count` | Number of objects |
| `snapshot_at(index, ...)` | Takes an object snapshot by index |
| `snapshot_by_handle(handle, ...)` | Takes an object snapshot by handle |
| `find_exact(path, ...)` | Calls the Profile-validated native UE lookup by exact UTF-8 object path on the Game thread only; returns `NOT_FOUND` while the object is not yet loaded |

`find_exact` is a field appended at the end of the version 1 table. Callers must first check that the field exists with `struct_size`; older hosts do not publish it.

Helper macros split a handle into index / serial:

```c
#define ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle)  ((uint32_t)((handle).id) - 1u)
#define ANOMALY_UE5_OBJECT_HANDLE_SERIAL(handle) ((uint32_t)((handle).id >> 32u))
```

## `anomaly.ue5.world`

- **ID**: `"anomaly.ue5.world"` · **version** 1 · **capability** `ue5-world`

```c
typedef struct AnomalyUe5WorldSnapshotV1 {
    uint32_t struct_size; uint32_t reserved;
    AnomalyGenerationHandleV1 handle; uint64_t change_sequence;
    uint32_t name_id; uint32_t flags;
} AnomalyUe5WorldSnapshotV1;
typedef struct AnomalyUe5WorldServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *current)(void* user, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *snapshot)(void* user, AnomalyGenerationHandleV1, AnomalyUe5WorldSnapshotV1*);
} AnomalyUe5WorldServiceV1;
```

`current` returns the generation handle of the current World; `snapshot` takes the World snapshot for that handle (including `change_sequence`).

---

## `anomaly.ue5.streaming-source`

- **ID**: `"anomaly.ue5.streaming-source"` · **version** 1 · **capability** `ue5-streaming-source`
- **Dependencies**: feature `ue5.streaming-source` (depends on `nte.player`, validator `ue5-streaming-source-layout-v1`, layout key `controller.streamingSourceVtableOffset`)

```c
#define ANOMALY_UE5_STREAMING_SOURCE_DURATION_V1_UNTIL_CLEARED 0u
typedef enum AnomalyUe5StreamingSourceOverrideFlagsV1 {
    ANOMALY_UE5_STREAMING_SOURCE_OVERRIDE_V1_NONE = 0,
    ANOMALY_UE5_STREAMING_SOURCE_OVERRIDE_V1_ROTATION = 1
} AnomalyUe5StreamingSourceOverrideFlagsV1;
typedef struct AnomalyUe5StreamingSourceOverrideV1 {
    uint32_t struct_size; uint32_t flags;
    double position[3];
    double rotation[3];
    uint32_t duration_milliseconds; uint32_t reserved;
} AnomalyUe5StreamingSourceOverrideV1;
typedef struct AnomalyUe5StreamingSourceServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *set_override)(void* user,
        const AnomalyUe5StreamingSourceOverrideV1* request);
    AnomalyStatusV1 (ANOMALY_CALL *clear_override)(void* user);
} AnomalyUe5StreamingSourceServiceV1;
```

`set_override` makes the local player controller's streaming source evaluate at that position, so the target area is loaded before the character moves there; `rotation` takes effect only when `ANOMALY_UE5_STREAMING_SOURCE_OVERRIDE_V1_ROTATION` is set (used for free camera view following). When `duration_milliseconds` is `UNTIL_CLEARED` the override is kept until `clear_override`; any other value expires automatically after that many milliseconds — even if the consumer forgets to clear it, the streaming source is not hijacked long term. Both calls must execute inside the Game callback domain, otherwise they return `CONFLICT`.

> [!NOTE]
> The host owns the **single** process-level streaming source hook, so multiple plugins can consume the service at the same time but share one override slot: the last request wins until it expires or is cleared. The only exception is the **teleport arrival window** — the host reserves the slot for that teleport, during which `set_override` from other consumers returns `CONFLICT` (they can request again once the window ends), so the streaming source at the teleport destination is not rewritten elsewhere. Consumers should call `clear_override` when they no longer need it (for example when the plugin stops or the camera stops flying). The service is published only when its layout key, validator and `nte.player` dependency all pass; otherwise calls return `UNAVAILABLE` and callers should degrade to direct operation (for example a synchronous teleport). The default preload mode of `anomaly.nte.player-teleport` is also built on this service.

> [!NOTE]
> Higher-level, NTE gameplay-oriented services (session events, player / camera, entity paging) are covered in [NTE services](nte-services.md).
