# UI services

Headers: `services/ui.h` (`anomaly.ui`) and `services/ui_resources.h` (`window` / `font` / `texture` / `input`). These are host-owned C facades — **no ImGui type, context, allocator or draw-list pointer ever crosses the ABI**. For the shared conventions, see the [API reference overview](README.md).

## `anomaly.ui`

- **ID**: `"anomaly.ui"` · **Version** 1 · **capability** `ui`

`anomaly.ui` publishes only `AnomalyUiServiceV1`. Except for `developer_mode_enabled`, the UI calls in the table are valid only inside the current `on_draw` callback; `developer_mode_enabled` can also be called from `on_update`.

### Common types

```c
#define ANOMALY_RGBA_V1(r, g, b, a) /* packs into uint32_t (R in the low bits) */

typedef struct AnomalyEspCameraV1 {
    uint32_t struct_size; uint32_t flags;
    double position[3]; double rotation[3];
    float horizontal_fov_degrees; uint32_t reserved;
} AnomalyEspCameraV1;
typedef struct AnomalyEspEntityBoundsV1 {
    uint32_t struct_size; uint32_t flags; double center[3]; double extent[3];
} AnomalyEspEntityBoundsV1;
typedef struct AnomalyEspBoxStyleV1 {
    uint32_t struct_size; uint32_t flags;         // AnomalyEspBoxFlagsV1
    uint32_t color_rgba; uint32_t outline_color_rgba;
    float thickness; float outline_thickness;
} AnomalyEspBoxStyleV1;
// AnomalyEspBoxFlagsV1: ANOMALY_ESP_BOX_V1_NONE = 0, ANOMALY_ESP_BOX_V1_OUTLINE = 1<<0
```

### Service table

```c
typedef struct AnomalyUiServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    void (ANOMALY_CALL *set_next_window_size)(void* user, float width, float height, uint32_t condition);
    int  (ANOMALY_CALL *begin_window)(void* user, AnomalyStringViewV1 title, int* open, uint32_t flags);
    void (ANOMALY_CALL *end_window)(void* user);
    void (ANOMALY_CALL *text)(void* user, AnomalyStringViewV1 text);
    int  (ANOMALY_CALL *button)(void* user, AnomalyStringViewV1 label, float width, float height);
    int  (ANOMALY_CALL *draw_entity_bbox)(void* user, const AnomalyEspCameraV1*, const AnomalyEspEntityBoundsV1*, const AnomalyEspBoxStyleV1*);
    int  (ANOMALY_CALL *checkbox)(void* user, AnomalyStringViewV1 label, int* value);
    int  (ANOMALY_CALL *slider_float)(void* user, AnomalyStringViewV1 label, float* value, float minimum, float maximum);
    int  (ANOMALY_CALL *color_edit4)(void* user, AnomalyStringViewV1 label, float rgba[4]);
    int  (ANOMALY_CALL *draw_entity_box3d)(void* user, const AnomalyEspCameraV1*, const AnomalyEspEntityBoundsV1*, const AnomalyEspBoxStyleV1*);
    int  (ANOMALY_CALL *draw_entity_label)(void* user, const AnomalyEspCameraV1*, const AnomalyEspEntityBoundsV1*, AnomalyStringViewV1 text, uint32_t color_rgba);
    void (ANOMALY_CALL *separator)(void* user);
    int  (ANOMALY_CALL *begin_child)(void* user, AnomalyStringViewV1 id, float width, float height, uint32_t flags);
    void (ANOMALY_CALL *end_child)(void* user);
    int  (ANOMALY_CALL *begin_table)(void* user, AnomalyStringViewV1 id, int32_t columns, uint32_t flags, float outer_width, float outer_height);
    void (ANOMALY_CALL *table_next_row)(void* user);
    int  (ANOMALY_CALL *table_next_column)(void* user);
    void (ANOMALY_CALL *end_table)(void* user);
    int  (ANOMALY_CALL *begin_menu)(void* user, AnomalyStringViewV1 label, int enabled);
    void (ANOMALY_CALL *end_menu)(void* user);
    void (ANOMALY_CALL *open_popup)(void* user, AnomalyStringViewV1 id);
    int  (ANOMALY_CALL *begin_popup_modal)(void* user, AnomalyStringViewV1 id, int* open, uint32_t flags);
    void (ANOMALY_CALL *end_popup)(void* user);
    void (ANOMALY_CALL *close_current_popup)(void* user);
    int  (ANOMALY_CALL *filter_match)(void* user, AnomalyStringViewV1 filter, AnomalyStringViewV1 value);
    uint32_t (ANOMALY_CALL *frame_state)(void* user);
    void (ANOMALY_CALL *set_next_window_size_constraints)(void* user, float minimum_width, float minimum_height, float maximum_width, float maximum_height);
    void (ANOMALY_CALL *get_window_size)(void* user, float* width, float* height);
    int  (ANOMALY_CALL *input_uint32)(void* user, AnomalyStringViewV1 label, uint32_t* value, uint32_t step, uint32_t step_fast);
    int  (ANOMALY_CALL *input_double)(void* user, AnomalyStringViewV1 label, double* value, double step, double step_fast);
    int  (ANOMALY_CALL *developer_mode_enabled)(void* user);
    int  (ANOMALY_CALL *input_text)(void* user, AnomalyStringViewV1 label, char* buffer, size_t buffer_capacity, uint32_t flags);
    int  (ANOMALY_CALL *button_enabled)(void* user, AnomalyStringViewV1 label, float width, float height, int enabled);
    void (ANOMALY_CALL *same_line)(void* user, float offset_from_start_x, float spacing);
    void (ANOMALY_CALL *set_cursor_pos_x)(void* user, float local_x);
    int  (ANOMALY_CALL *text_link)(void* user, AnomalyStringViewV1 label, AnomalyStringViewV1 url);
    int  (ANOMALY_CALL *begin_tab_bar)(void* user, AnomalyStringViewV1 id, uint32_t flags);
    int  (ANOMALY_CALL *begin_tab_item)(void* user, AnomalyStringViewV1 label, int* open, uint32_t flags, int enabled);
    void (ANOMALY_CALL *end_tab_item)(void* user);
    void (ANOMALY_CALL *end_tab_bar)(void* user);
} AnomalyUiServiceV1;
```

| Function | Description |
| --- | --- |
| `set_next_window_size(w, h, condition)` | Sets the size of the next window |
| `begin_window(title, open, flags)` / `end_window()` | Begins / ends a window, returns whether it is expanded |
| `text` / `button` / `button_enabled` / `checkbox` / `slider_float` / `color_edit4` / `input_*` | Basic widgets and bounded input |
| `same_line` / `set_cursor_pos_x` | Compact inline widgets and local horizontal positioning |
| `text_link(label, url)` | Shows a left-clickable text link; accepts only `http://` or `https://`, and after the click the host requests the default browser once drawing has finished |
| `begin_child` / `begin_table` / `begin_menu` / `begin_popup_modal` and the matching `end_*` | Scoped UI containers |
| `begin_tab_bar` / `begin_tab_item` and the matching `end_*` | Native tab containers; call the matching end function only when begin returned 1 |
| `filter_match` / `frame_state` / `developer_mode_enabled` | Current frame and session state queries |
| `draw_entity_bbox` | Projects an axis-aligned world box using the Unreal rotator convention and draws it in the foreground; returns 1 only when it is visible |
| `draw_entity_box3d` | Projects and draws all 12 edges of a world box |
| `draw_entity_label` | Projects the box and draws a centered label above its 2D bounds |

> [!NOTE]
> `draw_entity_*` (ESP drawing) requires the `entity-esp` capability. They are kept only for ABI v1 compatibility; new plugins should use the Game-thread draw callbacks of [`anomaly.ue5.ahud`](ue5-services.md#anomalyue5ahud). `Entity ESP` already follows that path. The `ui` that `AnomalyPluginDescriptorV1::on_draw` receives is an `AnomalyUiServiceV1*`.

Related enums:

```c
typedef enum AnomalyUiFrameStateV1 {   // bits of the frame_state() return value
    ANOMALY_UI_FRAME_V1_NONE = 0,
    ANOMALY_UI_FRAME_V1_ITEM_HOVERED = 1<<0, ANOMALY_UI_FRAME_V1_WINDOW_FOCUSED = 1<<1,
    ANOMALY_UI_FRAME_V1_ITEM_ACTIVE = 1<<2, ANOMALY_UI_FRAME_V1_WANT_CAPTURE_MOUSE = 1<<3,
    ANOMALY_UI_FRAME_V1_WANT_CAPTURE_KEYBOARD = 1<<4, ANOMALY_UI_FRAME_V1_WANT_TEXT_INPUT = 1<<5
} AnomalyUiFrameStateV1;
typedef enum AnomalyUiTextInputFlagsV1 {
    ANOMALY_UI_TEXT_INPUT_V1_NONE = 0, ANOMALY_UI_TEXT_INPUT_V1_DIGITS = 1<<0
} AnomalyUiTextInputFlagsV1;
typedef enum AnomalyUiTableFlagsV1 {
    ANOMALY_UI_TABLE_V1_NONE = 0, ANOMALY_UI_TABLE_V1_SIZING_FIXED_FIT = 1<<0
} AnomalyUiTableFlagsV1;
typedef enum AnomalyUiTabBarFlagsV1 {
    ANOMALY_UI_TAB_BAR_V1_NONE = 0,
    ANOMALY_UI_TAB_BAR_V1_REORDERABLE = 1<<0,
    ANOMALY_UI_TAB_BAR_V1_AUTO_SELECT_NEW_TABS = 1<<1,
    ANOMALY_UI_TAB_BAR_V1_NO_TAB_LIST_SCROLLING_BUTTONS = 1<<2,
    ANOMALY_UI_TAB_BAR_V1_NO_TOOLTIP = 1<<3,
    ANOMALY_UI_TAB_BAR_V1_FITTING_POLICY_RESIZE_DOWN = 1<<4,
    ANOMALY_UI_TAB_BAR_V1_FITTING_POLICY_SCROLL = 1<<5
} AnomalyUiTabBarFlagsV1;
typedef enum AnomalyUiTabItemFlagsV1 {
    ANOMALY_UI_TAB_ITEM_V1_NONE = 0,
    ANOMALY_UI_TAB_ITEM_V1_UNSAVED_DOCUMENT = 1<<0,
    ANOMALY_UI_TAB_ITEM_V1_SET_SELECTED = 1<<1,
    ANOMALY_UI_TAB_ITEM_V1_NO_CLOSE_WITH_MIDDLE_MOUSE_BUTTON = 1<<2,
    ANOMALY_UI_TAB_ITEM_V1_NO_PUSH_ID = 1<<3,
    ANOMALY_UI_TAB_ITEM_V1_NO_TOOLTIP = 1<<4
} AnomalyUiTabItemFlagsV1;
```

`begin_table` accepts only `AnomalyUiTableFlagsV1` in `flags`. Use `ANOMALY_UI_TABLE_V1_SIZING_FIXED_FIT` to fit column widths to their content instead of stretching the columns of one row to the available width. `same_line(0, spacing)` gives a fixed small gap between adjacent widgets; `set_cursor_pos_x` takes a window-local X coordinate. Check the trailing fields of the service table with `struct_size` before calling them, so the plugin stays compatible with V1 hosts that do not yet provide these optional widgets.

---

## UI resource services (`ui_resources.h`)

The following three resource services plus `anomaly.input` give plugins scoped access to windows, fonts, textures and input without touching ImGui types. All handles are bound to a generation.

### `anomaly.window`

- **ID**: `"anomaly.window"` · **Version** 1 · **capability** `ui-window`

```c
typedef struct AnomalyWindowSpecV1 {
    uint32_t struct_size; uint32_t flags;      // AnomalyWindowFlagsV1
    AnomalyStringViewV1 id; AnomalyStringViewV1 title;
    float initial_width, initial_height;
    float minimum_width, minimum_height, maximum_width, maximum_height;
    int32_t default_open; uint32_t reserved;
} AnomalyWindowSpecV1;
typedef struct AnomalyWindowStateV1 {
    uint32_t struct_size; uint32_t flags;
    float width, height; uint64_t ui_generation; int32_t open; uint32_t reserved;
} AnomalyWindowStateV1;
// AnomalyWindowFlagsV1: NONE=0, NO_SAVED_SETTINGS=1<<0, NO_COLLAPSE=1<<1

typedef struct AnomalyWindowServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *register_window)(void* user, const AnomalyWindowSpecV1*, AnomalyGenerationHandleV1*);
    AnomalyStatusV1 (ANOMALY_CALL *release_window)(void* user, AnomalyGenerationHandleV1);
    AnomalyStatusV1 (ANOMALY_CALL *set_open)(void* user, AnomalyGenerationHandleV1, int32_t open);
    AnomalyStatusV1 (ANOMALY_CALL *toggle)(void* user, AnomalyGenerationHandleV1);
    AnomalyStatusV1 (ANOMALY_CALL *state)(void* user, AnomalyGenerationHandleV1, AnomalyWindowStateV1*);
    AnomalyStatusV1 (ANOMALY_CALL *begin)(void* user, AnomalyGenerationHandleV1, uint32_t flags, int32_t* visible);
    AnomalyStatusV1 (ANOMALY_CALL *end)(void* user, AnomalyGenerationHandleV1);
} AnomalyWindowServiceV1;
```

The host manages a window's persistent layout and visibility; `begin` / `end` surround the window content inside `on_draw`. Once a plugin registers a managed window, the open/hidden state in the plugin catalog is driven as one window group under the same generation: closing the last window marks the plugin UI as hidden, and reopening it from the plugin catalog restores the `default_open` window; if no window declares `default_open`, the host restores a deterministic fallback window. Hiding the window group closes every window, and reopening a partially visible window group does not additionally open auxiliary windows that were originally closed.

### `anomaly.font`

- **ID**: `"anomaly.font"` · **Version** 1 · **capability** `ui-font`

```c
typedef struct AnomalyFontRequestV1 {
    uint32_t struct_size; uint32_t flags; AnomalyStringViewV1 relative_path;
    float size_pixels; uint32_t glyph_range;   // AnomalyGlyphRangeV1
    uint32_t reserved;
} AnomalyFontRequestV1;
typedef struct AnomalyFontStateV1 {
    uint32_t struct_size; uint32_t flags;       // AnomalyFontStateFlagsV1
    float effective_size_pixels; float scale;
    uint64_t device_generation; int32_t ready; uint32_t reserved;
} AnomalyFontStateV1;
// AnomalyGlyphRangeV1: DEFAULT=0, LATIN=1, CYRILLIC=2, JAPANESE=3, CHINESE_FULL=4
// AnomalyFontStateFlagsV1: NONE=0, QUEUED=1<<0, READY=1<<1, FAILED=1<<2, STALE_DEVICE=1<<3

typedef struct AnomalyFontServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *request)(void* user, const AnomalyFontRequestV1*, AnomalyGenerationHandleV1*);
    AnomalyStatusV1 (ANOMALY_CALL *release)(void* user, AnomalyGenerationHandleV1);
    AnomalyStatusV1 (ANOMALY_CALL *state)(void* user, AnomalyGenerationHandleV1, AnomalyFontStateV1*);
    AnomalyStatusV1 (ANOMALY_CALL *push)(void* user, AnomalyGenerationHandleV1);
    AnomalyStatusV1 (ANOMALY_CALL *pop)(void* user);
} AnomalyFontServiceV1;
```

Fonts load asynchronously: `request` returns a handle, `state` reports `QUEUED` / `READY` / `FAILED` / `STALE_DEVICE`; once the font is ready, switch fonts during drawing with `push` / `pop`. While a resource is not ready, the plugin should degrade its drawing.

### `anomaly.texture`

- **ID**: `"anomaly.texture"` · **Version** 1 · **capability** `ui-texture`

```c
typedef struct AnomalyTextureRequestV1 {
    uint32_t struct_size; uint32_t flags; AnomalyStringViewV1 relative_path;
    AnomalyByteSpanV1 encoded_bytes; uint32_t format;   // AnomalyTextureFormatV1
    uint32_t width; uint32_t height;                    // required for RGBA8; discovered while decoding encoded formats
    uint32_t reserved;
} AnomalyTextureRequestV1;
typedef struct AnomalyTextureStateV1 {
    uint32_t struct_size; uint32_t flags; uint32_t width, height;
    uint64_t device_generation; uint64_t byte_size;
} AnomalyTextureStateV1;
// AnomalyTextureFormatV1: AUTO=0, RGBA8=1
// AnomalyTextureStateFlagsV1: NONE=0, QUEUED=1<<0, READY=1<<1, FAILED=1<<2, STALE_DEVICE=1<<3

typedef struct AnomalyTextureServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *request)(void* user, const AnomalyTextureRequestV1*, AnomalyGenerationHandleV1*);
    AnomalyStatusV1 (ANOMALY_CALL *release)(void* user, AnomalyGenerationHandleV1);
    AnomalyStatusV1 (ANOMALY_CALL *state)(void* user, AnomalyGenerationHandleV1, AnomalyTextureStateV1*);
    AnomalyStatusV1 (ANOMALY_CALL *draw)(void* user, AnomalyGenerationHandleV1, float width, float height, uint32_t tint_rgba);
} AnomalyTextureServiceV1;
```

You can request a texture from a relative path or from embedded `encoded_bytes`; `draw` draws it in the current window at the given size and tint.

### `anomaly.input`

- **ID**: `"anomaly.input"` · **Version** 1 · **capability** `input`

```c
typedef struct AnomalyInputSnapshotV1 {
    uint32_t struct_size; uint32_t modifiers;   // AnomalyInputModifiersV1
    uint64_t sequence; uint64_t timestamp_milliseconds;
    float mouse_x, mouse_y; int32_t mouse_wheel;
    uint32_t capture_flags;                     // AnomalyInputCaptureFlagsV1
    uint8_t keys[32]; uint8_t mouse_buttons; uint8_t reserved[7];
} AnomalyInputSnapshotV1;
typedef struct AnomalyHotkeySpecV1 {
    uint32_t struct_size; uint32_t modifiers; uint32_t virtual_key;
    uint32_t flags;                             // AnomalyHotkeyFlagsV1
    AnomalyStringViewV1 id;
} AnomalyHotkeySpecV1;
typedef void (ANOMALY_CALL *AnomalyHotkeyCallbackV1)(void* user,
    AnomalyGenerationHandleV1 hotkey, const AnomalyInputSnapshotV1* snapshot);

typedef struct AnomalyInputServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *snapshot)(void* user, AnomalyInputSnapshotV1*);
    AnomalyStatusV1 (ANOMALY_CALL *was_pressed)(void* user, uint32_t virtual_key, int32_t* pressed);
    AnomalyStatusV1 (ANOMALY_CALL *register_hotkey)(void* user, const AnomalyHotkeySpecV1*,
        AnomalyHotkeyCallbackV1 callback, void* callback_user, AnomalyGenerationHandleV1*);
    AnomalyStatusV1 (ANOMALY_CALL *release_hotkey)(void* user, AnomalyGenerationHandleV1);
    AnomalyStatusV1 (ANOMALY_CALL *capture_state)(void* user, uint32_t* capture_flags);
} AnomalyInputServiceV1;
```

Flag enums:

```c
// AnomalyInputModifiersV1: NONE=0, SHIFT=1<<0, CONTROL=1<<1, ALT=1<<2, SUPER=1<<3
// AnomalyInputCaptureFlagsV1: NONE=0, MOUSE=1<<0, KEYBOARD=1<<1, TEXT=1<<2
// AnomalyHotkeyFlagsV1: NONE=0, ALLOW_EXTRA_MODIFIERS=1<<0,
//                       ALLOW_WHILE_UI_CAPTURED=1<<1, ONLY_WHILE_UI_CAPTURED=1<<2
```

`register_hotkey` registers a hotkey callback; modifiers match exactly by default, and the flags can relax that. `capture_state` reports whether the UI is currently capturing the mouse / keyboard / text.

For complete usage, see [`examples/hello_ui`](../../../examples/hello_ui/plugin.c).
