# NTE services

Corresponding header: `services/nte.h`. These are the **high-level NTE semantic services**: session events, player / camera, entity / Actor, character combat, skills, and snapshot metrics. They are published only after the corresponding symbols and runtime reflection shapes of the active [Profile](../user-guide/nte-profiles.md) pass validation; otherwise they stay `UNAVAILABLE` per Feature. For the general conventions, see the [API reference overview](README.md).

> [!IMPORTANT]
> A service table belongs to one Host lifecycle generation. Cached tables from a stopped or replaced generation only report `UNAVAILABLE` (scalar queries return 0), and non-zero cursors / generations do not survive a Host restart.

## Snapshot validity flags

Many NTE snapshots use the following in their `flags` field:

```c
typedef uint32_t AnomalyNteSnapshotFlagsV1;
#define ANOMALY_NTE_SNAPSHOT_V1_INVALID 0u
#define ANOMALY_NTE_SNAPSHOT_V1_VALID   (1u << 29u)
#define ANOMALY_NTE_SNAPSHOT_V1_STALE   (1u << 30u)
#define ANOMALY_NTE_SNAPSHOT_V1_PARTIAL (1u << 31u)
```

A snapshot without `VALID`, or one carrying `STALE`, cannot be used as the current state; `PARTIAL` means the remaining fields are still valid but the service is explicitly missing part of its data, and each service contract documents which items are missing. A stale generation must re-acquire the current frame / snapshot.

## `anomaly.nte.pickup`

- **ID**: `"anomaly.nte.pickup"` · **Version** 1 · **capability** `nte-pickup`

This service fixes nearby pickup and pickup confirmation inside the Host's NTE Adapter. Plugins only submit a radius and a per-request item limit; they never touch UE objects, UFunction, ProcessEvent, memory addresses, or Profile offsets.

```c
typedef struct AnomalyNtePickupRequestV1 {
    uint32_t struct_size; uint32_t flags;
    double radius; uint32_t maximum_items; uint32_t reserved;
} AnomalyNtePickupRequestV1;

typedef enum AnomalyNtePickupStateV1 {
    ANOMALY_NTE_PICKUP_V1_IDLE = 0,
    ANOMALY_NTE_PICKUP_V1_QUEUED = 1,
    ANOMALY_NTE_PICKUP_V1_CHECKING = 2,
    ANOMALY_NTE_PICKUP_V1_COMPLETE = 3
} AnomalyNtePickupStateV1;

typedef struct AnomalyNtePickupSnapshotV1 {
    uint32_t struct_size; uint32_t flags; uint64_t sequence;
    uint32_t state; uint32_t status;
    uint32_t nearby; uint32_t triggered; uint32_t confirmed;
    uint32_t checking; uint32_t unconfirmed; uint32_t skipped;
} AnomalyNtePickupSnapshotV1;

typedef struct AnomalyNtePickupServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *request_nearby)(
        void* user, const AnomalyNtePickupRequestV1* request);
    AnomalyStatusV1 (ANOMALY_CALL *snapshot)(
        void* user, AnomalyNtePickupSnapshotV1* snapshot);
} AnomalyNtePickupServiceV1;
```

`request_nearby` is allowed only on the Host's Game callback domain. Returning `OK` means the request is queued; the actual candidate scan and interaction happen on a later Game tick, and `snapshot` can be read from the Render/UI domain. The `radius` contract is 50--5000, `maximum_items` is 1--128, and `flags` and `reserved` must be 0. While the service is available and no request has been submitted, the snapshot is `VALID + IDLE + OK`.

`nearby` is the number of `HTRandomItemActor`, `PropBox_`, or `InteractBox_` within the radius; `triggered` is the number of successful `TriggerInteract` calls; `confirmed` comes from the object disappearing in a later full entity snapshot, a change in `bInteractFinish`, or a single `BPCanTryInteract == false` at the deadline; `checking` is the number still inside the confirmation window; `unconfirmed` is the number that are still interactable after the window closes or whose final state is unreadable; `skipped` aggregates candidates that are unreadable, not pickable, or failed `BPCanTryInteract`.

The confirmation path reads only the existing entity cache plus one state byte, at a minimum interval of 100 ms; it never calls reflection functions repeatedly inside the window. A single final `BPCanTryInteract` runs after at most 2 seconds. A timeout stays `OK + unconfirmed`; it never rewrites an already triggered request as `FAILED`, and never leaves a snapshot permanently in `CHECKING`.

Snapshot `flags` use `VALID`, `CHECKING_FLAG`, and `HAS_UNCONFIRMED`. `sequence` increments on every request, and a change of World, object registry, or Host generation clears the current request and returns `UNAVAILABLE`. The service is published only after the active Profile's `nte-pickup-layout-v1`, the UE5 ProcessEvent ABI, and the object/name/player/entity dependencies all pass validation. The `numParms`, `parmsSize`, all parameter offsets, and the entry layout of `TriggerInteract`, `BPCanTryInteract`, and `BPGetInteractEntries` all come from the active Profile; the Host never fills these layouts in from plugin-side constants.

---

## `anomaly.nte.ui-buttons`

- **ID**: `"anomaly.nte.ui-buttons"` · **Version** 1 · **capability** `nte-ui-buttons`

This service lets plugins click game UI buttons directly (UMG `Button`, CommonUI `CommonButtonBase`, HTGame `HTUI_Button`), as well as tabs / radio buttons (HTGame `HTUI_RadioBox`, `HTRadioBox`, UMG `CheckBox`) and list entries (HTGame `HTUI_ListItem`). The Host scans the widget tree, decides whether a button can be clicked, identifies the button under the mouse, and calls the widget's own click handler in the same order as a real click; **it does not synthesize any mouse or keyboard input**, and does not expose UE object pointers, UFunction, or Profile offsets to plugins.

```c
typedef struct AnomalyNteUiButtonsServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *status)(void* user, AnomalyNteUiButtonsStatusV1* status);
    AnomalyStatusV1 (ANOMALY_CALL *button_at)(void* user, uint64_t catalog_sequence,
        uint32_t index, AnomalyNteUiButtonSnapshotV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *window_at)(void* user, uint64_t catalog_sequence,
        uint32_t index, AnomalyNteUiWindowSnapshotV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *find)(void* user, const AnomalyNteUiButtonQueryV1* query,
        AnomalyNteUiButtonSnapshotV1* first, uint32_t* match_count);
    AnomalyStatusV1 (ANOMALY_CALL *request_scan)(void* user, AnomalyGenerationHandleV1* request);
    AnomalyStatusV1 (ANOMALY_CALL *request_pick)(void* user, AnomalyGenerationHandleV1* request);
    AnomalyStatusV1 (ANOMALY_CALL *request_click)(void* user,
        const AnomalyNteUiButtonClickRequestV1* click, AnomalyGenerationHandleV1* request);
    AnomalyStatusV1 (ANOMALY_CALL *request_snapshot)(void* user,
        AnomalyGenerationHandleV1 request, AnomalyNteUiButtonRequestSnapshotV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *pick_hit_at)(void* user, AnomalyGenerationHandleV1 request,
        uint32_t index, AnomalyNteUiButtonSnapshotV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *cancel)(void* user, AnomalyGenerationHandleV1 request);
} AnomalyNteUiButtonsServiceV1;
```

The full struct, reason-bit, and flag-bit definitions are in `services/nte.h`. Every entry point can be called from any thread.

### Request state machine

`request_scan`, `request_pick`, and `request_click` only enqueue the request in the Host queue and return a request handle; the actual work runs on later Game ticks in time slices (≤3 ms and ≤48 ProcessEvent calls per tick). Requests run one at a time, in submission order:

```text
QUEUED ──(dequeued on Game tick)──▶ RUNNING ──▶ COMPLETE(status)
   │                                  │
   └───────────── cancel ─────────────┴──▶ COMPLETE(CANCELLED)
```

- `request_snapshot` reads the request's `state` and final `status`. After `COMPLETE`, `status` is the result:
  | status | Meaning |
  | --- | --- |
  | `OK` | The scan published a new catalog / the pickup completed / the click was executed and (for HTUI buttons) accepted by the game |
  | `CONFLICT` | The button was not clickable at click time; `reasons` and `detail` give the reason and the panel that occludes it |
  | `NOT_FOUND` | The button handle is stale (the object was reclaimed, the slot was reused, or the object registry moved to a new generation) |
  | `FAILED` | The call threw, or the HTUI button discarded this click (for example, it is still inside the click interval) |
  | `UNAVAILABLE` | The Profile, the reflection types, or the object registry are unavailable |
  | `CANCELLED` | Cancelled by `cancel` |
- At most 32 requests may be open at the same time; once that limit is exceeded, submission returns `CONFLICT`. The 64 most recent completed requests are retained for querying.
- A click request completes on the tick that executes it; scans and pickups may span several ticks.

### Button catalog

A completed scan publishes an immutable catalog identified by `catalog_sequence`; `status` returns the current catalog ordinal and the button / UI-layer counts. `button_at` / `window_at` must pass the sequence they were enumerated with; once a new scan replaces the catalog, the old sequence returns `NOT_FOUND`, at which point you re-read `status`. The catalog is ordered clickable → not clickable → hidden.

Each button carries a `category` (`CLICKABLE` / `BLOCKED` / `HIDDEN`) and `reasons` bits: visibility-class reasons (the widget itself or a parent is hidden, fully transparent, a WidgetSwitcher is not on the current page, it is not the current widget of its UI layer, it is not attached to the UI, it is outside the viewport) map to `HIDDEN`, and the remaining reasons (disabled, not interactable, locked, not hit-testable, the UI is closing, **occluded by another panel**) map to `BLOCKED`. `cause` gives the widget or window name that corresponds to the first reason.

"Occluded by another panel" is decided by UI layer: the Host reads the currently displayed widget of each CommonUI layer, and a widget that is displayed and is either modal, pauses the game, is menu input exclusive, or requests hiding the main UI occludes buttons drawn below it in draw order; a widget that requests hiding the main UI also occludes every button inside the main UI (`HTUI_MainForm`). Prefer over-reporting occlusion: a false positive only costs one clickable button, while a false negative clicks a panel the player cannot see and leaves the game in an unpredictable state.

`find` filters the catalog by name, button text, containing window (the window / the owning UserWidget / any UserWidget on the path), and category mask, and returns the first match plus the match count.

### Clicking

The `button` handle of `request_click` comes from a catalog snapshot. Before executing, the Host re-checks object identity and **re-reads the UI layers to decide clickability again**: a panel opened after the scan intercepts the click just the same. When the button is not clickable, the request completes with `CONFLICT` and no game function is called; `ANOMALY_NTE_UI_BUTTON_CLICK_V1_FORCE` skips this decision (it still rejects stale handles).

The click sequence is fixed as press → release → click: CommonUI buttons call `HandleButtonPressed`, `HandleButtonReleased`, and `HandleButtonClicked`; UMG buttons call, in order, the functions bound to the `OnPressed`, `OnReleased`, and `OnClicked` delegates. `HTUI_Button` accepts a click only after its armed flag is set during the press phase, so the Host decides acceptance by whether the click time recorded on the button changed, and reports `PRESS_ARMED` / `CLICK_ACCEPTED` in `outcome`; a click the game does not accept completes with `FAILED`.

Tabs and list entries are listed by the widget itself (`kind` is `RADIO` and `LIST_ENTRY` respectively), and the `RadioBox` / `BlockBtn` / `Btn_Click` wrapped inside them are no longer listed separately:

| kind | Widget | Click | Lock check |
| --- | --- | --- | --- |
| `RADIO` | `HTUI_RadioBox` | `SetSelected(true, true)`, with the game broadcasting the selected event | `IsSystematicGameFeatureActivated` is false |
| `RADIO` | `HTRadioBox` / `HTCheckBox` | `HTRadioBox.SetSelected(true, true)`; a non-radio widget is handled as a `CheckBox` | None |
| `RADIO` | UMG `CheckBox` | `SetIsChecked(true)`, then call the `OnCheckStateChanged` binding with `true` | None |
| `LIST_ENTRY` | `HTUI_ListItem` | `OnBtnPressed` → `OnBtnReleased` → `OnBtnClicked` | `IsItemLocked` is true |

**An already selected tab still counts as clickable**: what the UI becomes after the click is decided by the current panel, not by the tab's selected state. If the widget ends up selected after the click, `outcome` reports `CLICK_ACCEPTED`. Visibility and occlusion checks are the same as for an ordinary button.

### Mouse picking

`request_pick` rescans first, then queries `UWidget::IsHovered` on every non-hidden button (the hover state Slate maintains from the real cursor). When it completes, `hit_count` is the number of hover hits and `pick_hit_at` reads them from innermost to outermost; index 0 is the innermost button the cursor actually points at. A button that is judged occluded but is hovered is also returned (reported as `BLOCKED`), which lets you spot occlusion false positives. When the cursor is not over the game window there are no hits.

### Example: claim a daily reward

```c
// Advance from on_update (Game domain): scan first, then find the button and click it, and finally confirm the click result.
AnomalyGenerationHandleV1 scan;
buttons->request_scan(buttons->user, &scan);
// ... poll on later ticks until request_snapshot(scan) returns COMPLETE + OK ...

AnomalyNteUiButtonQueryV1 query = {sizeof(query)};
query.name = (AnomalyStringViewV1){"BtnClaim", 8};
query.category_mask = ANOMALY_NTE_UI_BUTTON_QUERY_V1_CATEGORY(
    ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_CLICKABLE);
AnomalyNteUiButtonSnapshotV1 button = {sizeof(button)};
uint32_t matches = 0;
if (buttons->find(buttons->user, &query, &button, &matches).code == ANOMALY_STATUS_V1_OK) {
    AnomalyNteUiButtonClickRequestV1 click = {sizeof(click)};
    click.button = button.button;
    AnomalyGenerationHandleV1 request;
    buttons->request_click(buttons->user, &click, &request);
    // Next tick: if request_snapshot(request) is COMPLETE + OK, the game has accepted the click.
}
```

Multi-step flows such as opening a panel or switching tabs are chained by the plugin in the order scan → find → click → rescan; each step must wait for the previous request to complete and check its `status`.

### Publication conditions and degradation

The active Profile must declare the `nte.ui-buttons` Feature, the `nte-ui-buttons-layout-v1` validator, and the `ue5.names`, `ue5.objects`, `ue5.object-find`, and `ue5.process-event` dependencies; widget, CommonUI, and HTGame field offsets all come from Profile layout keys such as `widget.*`, `panelSlot.*`, `activatableWidget.*`, `htuiButton.*`, `htuiBase.*`, `checkBox.*`, `textBlock.*`, `htuiRadioBox.*`, and `htuiListItem.*`. When the reflection functions for tabs or list entries are missing, only that widget class is left out. Reflection types are resolved once per object registry generation, the first time it is ready, and the `READY` bit of `status` reports the result; without `UMG.Widget.IsHovered` picking is unavailable (`PICK_AVAILABLE` is 0) and everything else keeps working. When the object registry moves to a new generation, the catalog and all button handles are invalidated and unsettled requests end with `UNAVAILABLE` or `NOT_FOUND`; after the Host stops or restarts, old request handles are no longer addressable.

---

## `anomaly.nte.build`

- **ID**: `"anomaly.nte.build"` · **Version** 1 · **capability** `nte-build`

```c
typedef struct AnomalyNteBuildServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *build_id)(void* user, char* destination, size_t* inout_size);
    uint32_t (ANOMALY_CALL *feature_state)(void* user, AnomalyStringViewV1 feature_id);
} AnomalyNteBuildServiceV1;
```

`feature_state` returns `AnomalyFeatureStateV1` and is the query entry point into the NTE Feature Matrix. The current production Runtime has PE fingerprint disabled, so `build_id` returns an empty string; do not use it to decide whether a Feature is available.

---

## `anomaly.nte.esc-menu-button`

- **ID**: `"anomaly.nte.esc-menu-button"` · **Version** 1 · **capability** `nte-esc-menu-button`

This service only extends the **NTE ESC menu**; it is not a general menu or a general-purpose UI service. After a plugin registers a button, the NTE bridge layer appends it to the end of the current ESC menu's existing buttons, in registration order; the existing button count is not part of the ABI contract, so callers must not assume it is fixed at 25.

```c
typedef enum AnomalyNteEscMenuButtonResultV1 {
    ANOMALY_NTE_ESC_MENU_BUTTON_RESULT_V1_NONE = 0,
    ANOMALY_NTE_ESC_MENU_BUTTON_RESULT_V1_EXPAND_ANOMALY = 1
} AnomalyNteEscMenuButtonResultV1;

typedef enum AnomalyNteEscMenuButtonIconFormatV1 {
    ANOMALY_NTE_ESC_MENU_BUTTON_ICON_V1_NONE = 0,
    ANOMALY_NTE_ESC_MENU_BUTTON_ICON_V1_PNG = 1
} AnomalyNteEscMenuButtonIconFormatV1;

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
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *register_button)(
        void* user, const AnomalyNteEscMenuButtonSpecV1* spec,
        AnomalyNteEscMenuButtonCallbackV1 callback, void* callback_user,
        AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *unregister_button)(
        void* user, AnomalyGenerationHandleV1 handle);
} AnomalyNteEscMenuButtonServiceV1;
```

`flags` must be `ANOMALY_NTE_ESC_MENU_BUTTON_V1_NONE` and `reserved` must be 0. `id` must be unique within the same plugin owner; different plugins may use the same ID. `icon_format` may be `NONE` or `PNG`; `icon_bytes` for `PNG` is capped at 1 MiB, and `NONE` requires an empty byte span. The host copies `id`, the UTF-8 `label`, and the icon bytes, so the original memory can be released as soon as registration returns. The generation handle returned by a successful registration belongs only to that plugin generation; an explicit `unregister_button` or a plugin Scope revocation invalidates it and keeps the callback from outliving the generation lifecycle.

After the user clicks the button, the host invokes the callback supplied at registration on the NTE game thread, and the plugin can implement its own button event in that callback. Returning `NONE` triggers no additional host action; returning `EXPAND_ANOMALY` requests expanding the Anomaly management UI. If a custom PNG fails to import 3 times in a row within the same menu build, that button keeps being appended with the default icon of the NTE button widget; the next menu rebuild retries the custom icon. Anomaly's own default button uses the embedded `logo.png` and registers as the first item of the host's appended region; third-party plugins keep appending after it in successful registration order.

---

## `anomaly.nte.session`

- **ID**: `"anomaly.nte.session"` · **Version** 1 · **capability** `nte-session-snapshot`

```c
typedef enum AnomalyNteSessionStateV1 {
    ANOMALY_NTE_SESSION_V1_UNKNOWN = 0, ANOMALY_NTE_SESSION_V1_LOADING = 1,
    ANOMALY_NTE_SESSION_V1_WORLD_READY = 2
} AnomalyNteSessionStateV1;
typedef struct AnomalyNteSessionSnapshotV1 {
    uint32_t struct_size; uint32_t state; uint64_t sequence; AnomalyGenerationHandleV1 world;
} AnomalyNteSessionSnapshotV1;

typedef enum AnomalyNteSessionEventKindV1 {
    ANOMALY_NTE_SESSION_EVENT_V1_NONE = 0, ANOMALY_NTE_SESSION_EVENT_V1_WORLD_READY = 1,
    ANOMALY_NTE_SESSION_EVENT_V1_WORLD_CHANGED = 2, ANOMALY_NTE_SESSION_EVENT_V1_WORLD_UNAVAILABLE = 3
} AnomalyNteSessionEventKindV1;
typedef struct AnomalyNteSessionEventV1 {
    uint32_t struct_size; uint32_t kind;
    uint64_t sequence; uint64_t tick_sequence;
    AnomalyGenerationHandleV1 previous_world; AnomalyGenerationHandleV1 world;
} AnomalyNteSessionEventV1;

typedef struct AnomalyNteSessionServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *snapshot)(void* user, AnomalyNteSessionSnapshotV1*);
    AnomalyStatusV1 (ANOMALY_CALL *next_event)(void* user, uint64_t after_sequence, AnomalyNteSessionEventV1*);
    uint64_t (ANOMALY_CALL *latest_event_sequence)(void* user);
} AnomalyNteSessionServiceV1;
```

The event stream never exposes a World pointer; callers only keep an opaque, monotonically increasing cursor and generation handles. `next_event(after_sequence, ...)` returns the first retained event whose sequence is greater than `after_sequence`; a stale non-zero cursor and an empty future range both return `NOT_FOUND`.

---

## `anomaly.nte.player`

- **ID**: `"anomaly.nte.player"` · **Version** 1 · **capability** `nte-player-snapshot`

```c
typedef struct AnomalyNtePlayerSnapshotV1 {
    uint32_t struct_size; uint32_t flags; AnomalyGenerationHandleV1 handle;
    uint64_t sequence; double position[3];
} AnomalyNtePlayerSnapshotV1;
typedef struct AnomalyNtePlayerEspSnapshotV1 {
    uint32_t struct_size; uint32_t flags; AnomalyGenerationHandleV1 handle; uint64_t sequence;
    double bounds_center[3]; double bounds_extent[3];
    double camera_position[3]; double camera_rotation[3];
    float horizontal_fov_degrees; uint32_t reserved;
} AnomalyNtePlayerEspSnapshotV1;

typedef struct AnomalyNteCameraSnapshotV1 {
    uint32_t struct_size; uint32_t flags;
    AnomalyGenerationHandleV1 world; AnomalyGenerationHandleV1 player;
    uint64_t sequence; double position[3]; double rotation[3];
    float horizontal_fov_degrees; uint32_t reserved;
} AnomalyNteCameraSnapshotV1;
typedef struct AnomalyNtePlayerServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *snapshot)(void* user, AnomalyNtePlayerSnapshotV1*);
    AnomalyStatusV1 (ANOMALY_CALL *esp_snapshot)(void* user, AnomalyNtePlayerEspSnapshotV1*);
    AnomalyStatusV1 (ANOMALY_CALL *camera_snapshot)(void* user, AnomalyNteCameraSnapshotV1*);
    // Present only when struct_size covers them; requires the nte-player-hold capability.
    AnomalyStatusV1 (ANOMALY_CALL *hold_engage)(void* user);
    AnomalyStatusV1 (ANOMALY_CALL *hold_release)(void* user);
    AnomalyStatusV1 (ANOMALY_CALL *hold_snapshot)(void* user,
        AnomalyNtePlayerHoldSnapshotV1* snapshot);
} AnomalyNtePlayerServiceV1;
```

Camera data is available only after the active Profile has validated the optional `nte.player-esp` capability of the Player service. `world` identifies the scene, and `player` identifies the Pawn / Controller that supplied this camera sample; once either generation handle goes stale, the relationship is invalid.

---

## `anomaly.nte.player-teleport`

- **ID**: `"anomaly.nte.player-teleport"` · **Version** 1 · **capability** `nte-player-teleport`

```c
typedef struct AnomalyNtePlayerTeleportRequestV1 {
    uint32_t struct_size; uint32_t flags;
    AnomalyGenerationHandleV1 world; AnomalyGenerationHandleV1 player;
    double position[3];
} AnomalyNtePlayerTeleportRequestV1;
typedef struct AnomalyNtePlayerTeleportPreloadRequestV1 {
    uint32_t struct_size; uint32_t flags;
    double position[3];
    uint32_t duration_milliseconds; uint32_t reserved;
} AnomalyNtePlayerTeleportPreloadRequestV1;
typedef struct AnomalyNtePlayerTeleportServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *teleport)(void* user,
        const AnomalyNtePlayerTeleportRequestV1* request);
    AnomalyStatusV1 (ANOMALY_CALL *preload)(void* user,
        const AnomalyNtePlayerTeleportPreloadRequestV1* request);
    AnomalyStatusV1 (ANOMALY_CALL *cancel_preload)(void* user);
} AnomalyNtePlayerTeleportServiceV1;
```

> [!CAUTION]
> This is a **modifying** service. `teleport` is valid only inside the Game callback domain. With `flags` set to `0` (the default) it uses **preload mode**: the host first loads a framework streaming override at the destination (2000 ms by default, or the remaining window if `preload` was called earlier), **then performs the teleport inside the same call**, and then freezes the character for the duration of the window and clears the override. The order is deliberately reversed: put the character at the destination first, and only then point the streaming source at it — if you move the streaming source first and leave the character standing at the origin, the destination block loads while the ground under the origin is unloaded, and the fall settlement the game performs is anchored at the position the character occupies at that moment, so the whole origin→destination height difference is settled as a single fall and you die on landing (measured behavior). The character is always at the destination, so the fall anchor can only be the destination. The freeze written inside the window is the same as `anomaly.nte.player-hold` (gravity scale 0 + velocity 0, reasserted every tick), and gravity is handed back when the window expires; if the engine still reports the destination area as streaming at that point, the freeze is extended by at most 5 more seconds (`UWorldPartitionSubsystem::IsAllStreamingCompleted`), and when that query is unavailable the window is authoritative. During the window that streaming override slot belongs to this teleport, and `set_override` calls to `anomaly.ue5.streaming-source` from other consumers return `CONFLICT`. When the teleport itself fails, the freeze and the override are rolled back together and the error is returned to the caller unchanged; when preload is unavailable it degrades to a synchronous teleport and the degradation reason comes back in `message`. With `flags` set to `ANOMALY_NTE_PLAYER_TELEPORT_REQUEST_V1_IMMEDIATE` it does not touch the streaming source or the freeze at all, checks the position immediately after the call, and returns `FAILED` if the destination was not reached — **choose this when the destination area is already loaded**, for example Camera Tools' "Teleport to camera position" with "Scene loads around free camera" enabled. `preload` / `cancel_preload` request or cancel a preload window separately and in advance (for example before a teleport); when preload is unavailable they degrade to a synchronous teleport instead of failing. The host supplies `bSweep=false` and `bTeleport=true`, and does not expose UE object pointers or an `FHitResult` ABI. `world` and `player` must come from a current snapshot; stale handles are rejected. The service is published only when its engine `ProcessEvent` signature, ABI / reflection, dependencies, and game-thread gate all pass at the same time; **the Pawn-vtable fallback is forbidden**. Existing plugins can keep calling only `teleport`; new fields are detected through `struct_size`, and `service_version` stays 1.

---

## `anomaly.nte.player-hold`

- **ID**: `"anomaly.nte.player-hold"` · **Version** 1 · **capability** `nte-player-hold`
- **Flag bits**: `ANOMALY_NTE_PLAYER_HOLD_V1_HELD` = `1u << 0u`, `ANOMALY_NTE_PLAYER_HOLD_V1_REFUSED` = `1u << 1u`

```c
typedef struct AnomalyNtePlayerHoldSnapshotV1 {
    uint32_t struct_size; uint32_t flags; double gravity_scale; double velocity[3];
    uint32_t movement_mode; uint32_t reserved;
} AnomalyNtePlayerHoldSnapshotV1;

typedef struct AnomalyNtePlayerHoldServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *engage)(void* user);
    AnomalyStatusV1 (ANOMALY_CALL *release)(void* user);
    AnomalyStatusV1 (ANOMALY_CALL *snapshot)(void* user,
        AnomalyNtePlayerHoldSnapshotV1* snapshot);
} AnomalyNtePlayerHoldServiceV1;
```

> [!CAUTION]
> This is a **modifying** service. The host resolves the local character through the same reflection chain as the other features; `engage` writes two things: gravity scale to zero and velocity to zero; `release` zeroes velocity again and writes the original gravity scale back, so the character resumes gravity where it stands.
>
> **It is only responsible for "holding the character in place", not for changing the state the character is in.** Changing the movement mode to `MOVE_Flying` inside the window was tried once (the engine does not leave that mode on its own, and `PhysFlying` needs no ground), but in practice the client writes its own mode back every frame, the mode on the panel flips between `3` and `5`, and the benefit was zero. What actually decides life and death is **where the character is standing at that moment**: the fall the game settles is anchored at the position the character occupied when the fall began, so as long as the character stays at the destination the height difference is never settled (see `anomaly.nte.player-teleport`: teleport first, then freeze).
>
> `engage` is idempotent; calling `release` without `engage` is not an error. When the caller is on the Game callback domain the result is returned directly (including the reason for a refusal), while requests from other threads (for example a UI draw callback) are queued to a Game tick and the result shows up in the next `snapshot`. `snapshot` returns the `HELD` / `REFUSED` flags plus the live gravity scale, velocity, and movement mode (an `EMovementMode` value, where `3` is `MOVE_Falling`; this is a **read-only** diagnostic used to observe what the game itself is doing). At no point does it expose UE object pointers to plugins.

---

## `anomaly.nte.map-landmarks`

- **ID**: `"anomaly.nte.map-landmarks"` · **Version** 1 · **capability** `nte-map-landmarks`
- **ID limit**: `ANOMALY_NTE_MAP_LANDMARK_V1_ID_MAX_BYTES` = 128 UTF-8 bytes
- **World name limit**: `ANOMALY_NTE_MAP_LANDMARK_V1_WORLD_MAX_UTF8_BYTES` = 2048 UTF-8 bytes

```c
typedef uint32_t AnomalyNteMapLandmarkFlagsV1;
#define ANOMALY_NTE_MAP_LANDMARK_V1_VALID                  (1u << 0u)
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
    AnomalyStatusV1 (ANOMALY_CALL *teleport)(
        void* user, const AnomalyNteMapLandmarkTeleportRequestV1* request);
} AnomalyNteMapLandmarksServiceV1;
```

The host builds the catalog from the `TeleportPoint` DataTable validated by the active Profile, enumerating only landmarks whose `CanTeleport` is true. Each snapshot contains the DataTable row's `TeleportID`, the `BelongsLevel` world it belongs to, the world position, the floor, and the point type; when `DESTINATION_OVERRIDDEN` is set, `destination` is the override position, otherwise it is the same as `world_position`. Both text arrays are always NUL-terminated.

Catalog construction uses the ABI-validated `StaticFindObject` on the Game thread to resolve `GetGameData` and `MapIconTransfer` exactly; it does not linearly scan GObjects every tick. A successful catalog is cached and rebuilt only after the object registry generation changes; reading `sequence()`, `count()`, or `snapshot_at()` does not trigger a rescan.

The catalog is immutable within the same non-zero `sequence`. Consumers read `sequence()` and `count()` first, then call `snapshot_at()` over `[0, count)`; the `sequence` of the snapshot itself must match the one from the start of the read. If the sequence changes during the read, discard that batch and enumerate again. `sequence() == 0` means the catalog is not ready yet or the Feature is unavailable.

> [!CAUTION]
> `teleport` is a modifying call and is valid only inside the Game callback domain. The request must pass back the `sequence` and index of the current catalog; a stale sequence or an out-of-range index returns `NOT_FOUND`; `flags` must be 0 and `mode` may only take the two enum values above. The host re-resolves `TeleportID` in the current catalog and executes it through the validated `HTPlayerState.MapIconTransfer` / `ProcessEvent` bridge. Plugins must not pass UE object pointers, raw DataTable row addresses, or self-invented teleport IDs across the ABI.

The service depends on `nte.player`, `ue5.names`, `ue5.objects`, `ue5.object-find`, and `ue5.process-event`, and is gated by `nte-map-landmarks-layout-v1` validation. When any dependency or reflection contract does not hold, the service stays `UNAVAILABLE`.

---

## `anomaly.nte.navigation`

- **ID**: `"anomaly.nte.navigation"` · **Version** 1 · **capability** `nte-navigation`

```c
typedef struct AnomalyNteNavigationServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *move_to_location)(
        void* user, const double destination[3]);
    AnomalyStatusV1 (ANOMALY_CALL *stop_movement)(void* user);
} AnomalyNteNavigationServiceV1;
```

> [!CAUTION]
> This is a **modifying** service, and both calls are valid only inside the Game callback domain. `move_to_location` calls the native `HTUtil.MoveToPointByTransform` with the current local Controller's `ControlRotation`, always passing `ForceWalk=false`, `AutoControl=true`, `bHideUI=false`, `ProtectTime=0`, and `bUsingPathFinding=true`. The host overrides Patrol input capture only for the duration of that call, so the character keeps native turning, movement, and actions while the mouse and UI stay unlocked. `stop_movement` calls the native `StopMovement` of the current Controller. The host does not expose UE object pointers; the service is published only after ProcessEvent, the reflection layout, the input function ABI, and all dependencies are validated together.

---

## `anomaly.nte.entities`

- **ID**: `"anomaly.nte.entities"` · **Version** 1 · **capability** `nte-entity-snapshot`
- **Page capacity limit**: `ANOMALY_NTE_ENTITY_PAGE_V1_MAX_CAPACITY` = 256

```c
typedef enum AnomalyNteEntityFlagsV1 {
    ANOMALY_NTE_ENTITY_V1_NONE = 0, ANOMALY_NTE_ENTITY_V1_STATIC = 1<<0,
    ANOMALY_NTE_ENTITY_V1_STATIONARY = 1<<1, ANOMALY_NTE_ENTITY_V1_MOVABLE = 1<<2,
    ANOMALY_NTE_ENTITY_V1_LOCAL_PLAYER = 1<<3
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
```

```c
typedef struct AnomalyNteEntityPageRequestV1 {
    uint32_t struct_size; uint32_t flags;      // flags reserved, must be 0
    uint64_t generation;                        // 0 selects the current cached frame
    uint32_t offset; uint32_t capacity;         // capacity ≤ 256
    uint64_t class_id;
    uint32_t class_name_id; uint32_t entity_name_id;
    uint32_t required_flags; uint32_t excluded_flags;  // evaluated against AnomalyNteEntityFlagsV1
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
    AnomalyStatusV1 (ANOMALY_CALL *frame)(void* user, AnomalyNteEntityFrameV1*);
    AnomalyStatusV1 (ANOMALY_CALL *snapshot_at)(void* user, uint64_t generation, uint32_t index, AnomalyNteEntitySnapshotV1*);
    AnomalyStatusV1 (ANOMALY_CALL *class_name_utf8)(void* user, uint64_t class_id, char* destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *entity_name_utf8)(void* user, uint64_t entity_id, char* destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *page)(void* user, const AnomalyNteEntityPageRequestV1*, AnomalyNteEntitySnapshotV1*, AnomalyNteEntityPageResultV1*);
    AnomalyStatusV1 (ANOMALY_CALL *component_bounds)(void* user, AnomalyGenerationHandleV1 entity, AnomalyStringViewV1 property_name, AnomalyNteEntityComponentBoundsV1*);
    AnomalyStatusV1 (ANOMALY_CALL *bool_property)(void* user, AnomalyGenerationHandleV1 entity, AnomalyStringViewV1 property_name, AnomalyNteEntityBoolPropertyV1*);
    AnomalyStatusV1 (ANOMALY_CALL *fname_property_utf8)(void* user, AnomalyGenerationHandleV1 entity, AnomalyStringViewV1 property_name, char* destination, size_t* inout_size);
} AnomalyNteEntitiesServiceV1;
```

`frame` takes the current immutable entity frame (including the camera and the `generation`); name resolution only reads the current frame cache and never reads live game memory during the call. A page `generation` of 0 selects the current frame, and later pages must pass the returned generation back; a stale non-zero generation returns `NOT_FOUND`. The three bounded reflection reads are valid only inside the Game callback domain, do not expose UE object addresses, and validate the field shape before reading.

---

## `anomaly.nte.actors`

- **ID**: `"anomaly.nte.actors"` · **Version** 1 · **capability** `nte-actor-snapshot`

`AnomalyNteActorsServiceV1` has the same function shape as `AnomalyNteEntitiesServiceV1` but is used for **Actor discovery**. The first `frame` request for a World scans every loaded UWorld level, then rescans at the host actor sampling interval (`Performance/ActorSnapshotTickInterval`, 60 ticks by default) and rescans immediately when the World changes; reflection reads are likewise valid only inside the Game callback domain.

Rescanning is necessary: if scanning only happened when the World changes, a large world that keeps the same World for the whole session would freeze the snapshot forever — entries for destroyed actors would linger and newly spawned actors would never become visible.

Actor discovery is deliberately separated from the high-frequency entity snapshot — the former targets full enumeration, the latter targets per-frame sampling.

---

## `anomaly.nte.combat`

- **ID**: `"anomaly.nte.combat"` · **Version** 1 · **capability** `nte-combat-read`

```c
typedef struct AnomalyNteCombatantSnapshotV1 {
    uint32_t struct_size; uint32_t flags; uint64_t sequence;
    AnomalyGenerationHandleV1 world;
    AnomalyGenerationHandleV1 character;
    AnomalyGenerationHandleV1 target;
    double hp; double max_hp; double shield;
} AnomalyNteCombatantSnapshotV1;

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
```

`current_combatant` returns the current player character's HP, max HP, shield, death state, and attack target. `DEAD` is a low-bit flag specific to the combatant; `VALID / STALE / PARTIAL` keep using the common high bits. When the target object cannot be resolved, the target handle is zero and the snapshot is marked `PARTIAL`; a UObject address is never exposed.

Damage collection uses the exact native broadcast template of `AHTAbilityCharacter::CharacterOnDamaged`. The broadcast arguments directly provide the `FHTDamageEvent`, the damaged character, the damage instigating character, and the causer; `final_damage` is the integer-rounded value of `FHTDamageEvent::Damage`, and the event carries `ANOMALY_NTE_DAMAGE_V1_CHARACTER_EVENT`. `source_id` comes from the `DamageGEDef` weak object of the same event. Client display values and hit locations are still provided only when the corresponding data source is available.

Critical state is recovered as a synchronous query inside the native damage callback: through the validated `K2_GetAbilitySystemComponent` and `CurrentDamageIsCrit` reflection bindings, the victim is checked first and then the attacker, and if either returns critical that damage is marked. The query completes before the ring slot is reserved, and the result is copied into the queue along with the damage data. Only when every non-null participant query succeeds and returns false is the damage marked as confirmed non-critical; when a query fails the state stays unknown, and the already copied damage tags remain available as a fallback source for a definite critical. This path performs no FText name conversion.

`ANOMALY_NTE_DAMAGE_V1_CRITICAL_VALID` and `ANOMALY_NTE_COMBAT_EVENT_V1_CRITICAL_VALID` mean the critical state is known. Only when that bit is present does an unset `CRITICAL` mean confirmed non-critical; without the valid bit it must not be counted as confirmed non-critical. New flags do not change the service table or the event struct layout.

```c
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

typedef struct AnomalyNteCombatStatisticsV1 {
    uint32_t struct_size; uint32_t flags;
    uint64_t through_sequence;
    uint64_t hit_count; uint64_t critical_count; uint64_t head_hit_count;
    int64_t display_damage_total;
    int64_t basic_damage_total;
    int64_t final_damage_total;
} AnomalyNteCombatStatisticsV1;
```

`next_damage_event(after_sequence, ...)` returns the first retained sequence that is greater. Fixed ring overflow, a World switch, or a Host restart makes an old non-zero cursor stale; a stale cursor and having no new events both return `NOT_FOUND`. The caller can compare `latest_damage_sequence`: if the latest sequence is greater than the cursor but the next event still cannot be fetched, re-establish the retention window cursor from 0.

`statistics` requires the current `world` and can filter by `source_id`, character, and direction; a zero `character` / `source_id` means that filter is not applied. The statistics cover the host retention window of the current World; `PARTIAL` is set when the ring overflowed or resolution dropped entries earlier, `OVERFLOW` is set as well when a 64-bit accumulator saturates, and it never wraps around. `source_name_utf8` returns the FName cached in the current combat generation or the exact `GameplayEffect` object path. `participant_path_utf8` returns the attacker/victim UObject path cached on the game thread when the damage was collected; a stale generation or a handle that did not take part in a collected event returns `NOT_FOUND`, and the service call itself never reads a UObject across threads.

```c
typedef struct AnomalyNteCombatServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *current_combatant)(void*, AnomalyNteCombatantSnapshotV1*);
    uint64_t (ANOMALY_CALL *latest_damage_sequence)(void*);
    AnomalyStatusV1 (ANOMALY_CALL *next_damage_event)(void*, uint64_t, AnomalyNteDamageEventV1*);
    AnomalyStatusV1 (ANOMALY_CALL *statistics)(void*, const AnomalyNteCombatStatisticsRequestV1*, AnomalyNteCombatStatisticsV1*);
    AnomalyStatusV1 (ANOMALY_CALL *source_name_utf8)(void*, uint64_t, char*, size_t*);
    AnomalyStatusV1 (ANOMALY_CALL *participant_path_utf8)(void*, AnomalyGenerationHandleV1, char*, size_t*);
    uint64_t (ANOMALY_CALL *latest_event_sequence)(void*);
    AnomalyStatusV1 (ANOMALY_CALL *next_event)(void*, uint64_t, AnomalyNteCombatEventV1*);
    AnomalyStatusV1 (ANOMALY_CALL *event_name_utf8)(void*, const AnomalyNteCombatEventV1*, char*, size_t*);
    AnomalyStatusV1 (ANOMALY_CALL *participant_display_name_utf8)(void*, AnomalyGenerationHandleV1, char*, size_t*);
} AnomalyNteCombatServiceV1;
```

`latest_event_sequence` / `next_event` provide a combat event stream that covers damage, healing, and Buffs. `event_name_utf8` returns the display name of an event and `participant_display_name_utf8` returns the display name of a participant, which is distinct from the source identity and object path of `source_name_utf8` / `participant_path_utf8`. Display names are filled in deferred during the Game update; a valid request with no cached name yet returns `NOT_FOUND`. A missing name does not affect damage events or values, so consumers should keep the record and display the path, the ID, or a placeholder, then retry the name query at a moderate rate during Game updates. Before accessing entries appended to the end of the v1 service table, check whether `struct_size` covers the corresponding field.

Monster names prefer a specific scene entry and its dedicated text key, and only fall back to the base name when that fails. Text keys tolerate `_BP` / scene suffixes, zero-padded IDs, and the `_Name` / `_name` difference while keeping the variant ID and dedicated name priority; when several scene entries give different base names, that scene base-name fallback is not used. DataTable name fields must pass `TextProperty` type, size, and offset validation, and the icon table is not used as a name source.

FText decoding follows the optional `ue5.ftext` validation and read-only rules of the [UE5 name services](ue5-services.md); it reads the source text from an existing display cache or a loaded text table and does not call `Conv_TextToString`. Full name lookup still keeps the validated reflection queries for the character ID, monster static data, and text table references, so the whole name lookup path cannot be treated as a pure memory read.

Consumers should query the combat service from `on_update` in the Game domain: compare `latest_damage_sequence` first, call `next_damage_event` a bounded number of times only when the sequence changed, and resolve source, attacker, and victim once each when accepting a new event. HP, statistics, and resolution results should be converted immediately into an immutable snapshot owned by the plugin and directly drawable. The Render domain `on_draw` only copies that local snapshot and calls UI services; it does not query the combat service, does not advance the cursor, and does not resolve names. For the full consumption boundary, see [`examples/nte_combat_demo`](../../../examples/nte_combat_demo/plugin.cpp).

---

## `anomaly.nte.skills`

- **ID**: `"anomaly.nte.skills"` · **Version** 1 · **capability** `nte-skills-read`
- **Page capacity limit**: `ANOMALY_NTE_SKILL_PAGE_V1_MAX_CAPACITY` = 128

```c
typedef struct AnomalyNteSkillFrameV1 {
    uint32_t struct_size; uint32_t flags;
    uint64_t generation; uint64_t sequence;
    AnomalyGenerationHandleV1 character;
    uint32_t skill_count; uint32_t reserved;
} AnomalyNteSkillFrameV1;

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
```

`frame` pins the current immutable skill frame, and `snapshot_at` and `page` only access the host cache. The first page request may use generation 0, and later requests must pass the result generation back; a skill reorder keeps the handles for the same set of spec identities, while a skill removal, re-grant, character / World switch, or spec identity change produces a new generation that makes old handles return `NOT_FOUND`.

A skill handle is a host-generated opaque identity and is not the `FGameplayAbilitySpecHandle`. `ability_class` is likewise a generation handle and can be passed to `ability_path_utf8` and `ability_display_name_utf8`; it must not be interpreted as a UE address. v1 already publishes level, input, and the active / input-pressed / remove states; the cooldown tag shape has not been validated independently, so skills currently carry `PARTIAL` and not `COOLDOWN_VALID`, and both cooldown float fields are 0.

```c
typedef struct AnomalyNteSkillsServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *frame)(void*, AnomalyNteSkillFrameV1*);
    AnomalyStatusV1 (ANOMALY_CALL *snapshot_at)(void*, uint64_t, uint32_t, AnomalyNteSkillSnapshotV1*);
    AnomalyStatusV1 (ANOMALY_CALL *page)(void*, const AnomalyNteSkillPageRequestV1*, AnomalyNteSkillSnapshotV1*, AnomalyNteSkillPageResultV1*);
    AnomalyStatusV1 (ANOMALY_CALL *ability_path_utf8)(void*, AnomalyGenerationHandleV1, char*, size_t*);
    AnomalyStatusV1 (ANOMALY_CALL *snapshot_by_handle)(void*, AnomalyGenerationHandleV1, AnomalyNteSkillSnapshotV1*);
    AnomalyStatusV1 (ANOMALY_CALL *ability_display_name_utf8)(void*, AnomalyGenerationHandleV1, char*, size_t*);
} AnomalyNteSkillsServiceV1;
```

`ability_display_name_utf8` returns the cached display name of the current skill class, or `NOT_FOUND` when it has not been resolved yet. An unavailable name does not mean the skill is invalid; consumers can display the short path name or the skill ID first and decide whether to offer an activate action from the current skill snapshot and the activation service state. This entry point likewise requires checking the service table's `struct_size`.

---

## `anomaly.nte.skill-invocation`

- **ID**: `"anomaly.nte.skill-invocation"` · **Version** 1 · **capability** `nte-skill-invocation`

```c
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
```

> [!CAUTION]
> This is a standalone **modifying** service and can only be called from the Game callback domain. `world`, `character`, and `skill` must come from a current combat / skills snapshot, and `flags` must be 0. Before the call the host re-validates the current ASC, spec handle, ability CDO, and ability class identity, and then requests activation only through the validated `HTTryActivateAbilityByClass` entry point; it accepts no arbitrary path, UFunction, argument buffer, target, or location.

`activate` returning `OK` means the reflection bridge call completed normally, and the bool the game returns is written to `accepted`. `accepted == 0` means the game rejected this request, not that the ABI call failed; a non-Game thread returns `CONFLICT`, a stale generation returns `NOT_FOUND`, and the host never retries automatically. Render domain draw callbacks can only write user intent into a plugin-owned queue, and the Game domain `on_update` then calls the service.

---

## `anomaly.nte.metrics`

- **ID**: `"anomaly.nte.metrics"` · **Version** 1 · **capability** `nte-snapshot-metrics`

```c
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
    AnomalyStatusV1 (ANOMALY_CALL *snapshot)(void* user, AnomalyNteSnapshotMetricsV1*);
} AnomalyNteMetricsServiceV1;
```

`snapshot` reports the **host's** sampling work metrics (snapshot cost, player / entity refresh and cache hit counts, and so on) for diagnostics rather than per-plugin enumeration. For usage, see [`examples/nte_inspector`](../../../examples/nte_inspector/plugin.cpp).
