# UI 服务

对应头文件：`services/ui.h`（`anomaly.ui`）与 `services/ui_resources.h`（`window` / `font` / `texture` / `input`）。这些是宿主拥有的 C facade——**没有任何 ImGui 类型、context、allocator 或 draw-list 指针跨越 ABI**。通用约定见 [API 参考总览](README.md)。

## `anomaly.ui`

- **ID**：`"anomaly.ui"` · **版本** 1 · **capability** `ui`

`anomaly.ui` 只发布 `AnomalyUiServiceV1`。除 `developer_mode_enabled` 外，表中的 UI 调用仅在当前 `on_draw` 回调内有效；`developer_mode_enabled` 也可在 `on_update` 中调用。

### 通用类型

```c
#define ANOMALY_RGBA_V1(r, g, b, a) /* 打包为 uint32_t（R 低位） */

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

### 服务表

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
} AnomalyUiServiceV1;
```

| 函数 | 说明 |
| --- | --- |
| `set_next_window_size(w, h, condition)` | 设置下一个窗口尺寸 |
| `begin_window(title, open, flags)` / `end_window()` | 开始 / 结束窗口，返回是否展开 |
| `text` / `button` / `button_enabled` / `checkbox` / `slider_float` / `color_edit4` / `input_*` | 基础控件与有界输入 |
| `same_line` / `set_cursor_pos_x` | 紧凑的内联控件和局部水平定位 |
| `text_link(label, url)` | 显示左键可点的文本链接；仅接受 `http://` 或 `https://`，点击后由宿主在绘制结束后请求默认浏览器打开 |
| `begin_child` / `begin_table` / `begin_menu` / `begin_popup_modal` 及对应 `end_*` | 作用域 UI 容器 |
| `filter_match` / `frame_state` / `developer_mode_enabled` | 当前帧和会话状态查询 |
| `draw_entity_bbox` | 用 Unreal rotator 约定投影一个轴对齐世界盒并在前景绘制；仅当可见时返回 1 |
| `draw_entity_box3d` | 投影并绘制世界盒的全部 12 条棱 |
| `draw_entity_label` | 投影盒并在其 2D 边界上方绘制居中标签 |

> [!NOTE]
> `draw_entity_*`（ESP 绘制）需要 `entity-esp` capability。它们仅为 ABI v1 兼容而保留；新插件应使用
> [`anomaly.ue5.ahud`](ue5-services.md#anomalyue5ahud) 的 Game-thread 绘制回调。`Entity ESP` 已采用该路径。
> `AnomalyPluginDescriptorV1::on_draw` 收到的 `ui` 就是 `AnomalyUiServiceV1*`。

相关枚举：

```c
typedef enum AnomalyUiFrameStateV1 {   // frame_state() 返回值的位
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
```

`begin_table` 的 `flags` 只接受 `AnomalyUiTableFlagsV1`。使用
`ANOMALY_UI_TABLE_V1_SIZING_FIXED_FIT` 让列宽贴合内容，而不是把同一行的列拉伸到可用宽度。
`same_line(0, spacing)` 可在相邻控件之间使用固定小间隔；`set_cursor_pos_x` 接受窗口本地 X 坐标。
服务表尾部字段必须先用 `struct_size` 检查后再调用，以兼容尚未提供这些可选控件的 V1 宿主。

---

## UI 资源服务（`ui_resources.h`）

以下三个资源服务与 `anomaly.input` 让插件拥有 scoped 的窗口、字体、纹理与输入，无需接触 ImGui 类型。所有 handle 绑定 generation。

### `anomaly.window`

- **ID**：`"anomaly.window"` · **版本** 1 · **capability** `ui-window`

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

宿主管理窗口的持久布局与可见性；`begin` / `end` 在 `on_draw` 内包围窗口内容。插件一旦注册
managed window，插件列表中的打开/隐藏状态就由同一 generation 下的窗口组统一驱动：关闭最后一个
窗口会把插件 UI 标记为隐藏，从插件列表重新打开时会恢复 `default_open` 窗口；如果没有窗口声明
`default_open`，宿主会恢复一个确定的回退窗口。隐藏窗口组会关闭全部窗口，重新打开已部分可见的
窗口组不会额外打开原本关闭的辅助窗口。

### `anomaly.font`

- **ID**：`"anomaly.font"` · **版本** 1 · **capability** `ui-font`

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

字体异步加载：`request` 返回句柄，`state` 报告 `QUEUED` / `READY` / `FAILED` / `STALE_DEVICE`；就绪后用 `push` / `pop` 在绘制中切换字体。资源未就绪时插件应降级绘制。

### `anomaly.texture`

- **ID**：`"anomaly.texture"` · **版本** 1 · **capability** `ui-texture`

```c
typedef struct AnomalyTextureRequestV1 {
    uint32_t struct_size; uint32_t flags; AnomalyStringViewV1 relative_path;
    AnomalyByteSpanV1 encoded_bytes; uint32_t format;   // AnomalyTextureFormatV1
    uint32_t width; uint32_t height;                    // RGBA8 必填；编码格式解码时发现
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

可从相对路径或内嵌 `encoded_bytes` 请求纹理，`draw` 在当前窗口按尺寸与 tint 绘制。

### `anomaly.input`

- **ID**：`"anomaly.input"` · **版本** 1 · **capability** `input`

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

标志枚举：

```c
// AnomalyInputModifiersV1: NONE=0, SHIFT=1<<0, CONTROL=1<<1, ALT=1<<2, SUPER=1<<3
// AnomalyInputCaptureFlagsV1: NONE=0, MOUSE=1<<0, KEYBOARD=1<<1, TEXT=1<<2
// AnomalyHotkeyFlagsV1: NONE=0, ALLOW_EXTRA_MODIFIERS=1<<0,
//                       ALLOW_WHILE_UI_CAPTURED=1<<1, ONLY_WHILE_UI_CAPTURED=1<<2
```

`register_hotkey` 注册热键回调；默认修饰键精确匹配，可用标志放宽。`capture_state` 报告 UI 是否正在捕获鼠标 / 键盘 / 文本。

完整用法见 [`examples/hello_ui`](../../examples/hello_ui/plugin.c)。
