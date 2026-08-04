# UE5 服务

对应头文件：`services/ue5.h`。这些是**引擎通用**的适配服务，不含 NTE 专用字段。它们只在活动 [Profile](../user-guide/nte-profiles.md) 的相应符号验证通过后发布，否则按 Feature 保持 `UNAVAILABLE`。通用约定见 [API 参考总览](README.md)。

> [!IMPORTANT]
> 服务表属于一个 Host 生命周期 generation；来自已停止 generation 的缓存表只报告 `UNAVAILABLE`（标量查询返回 0）。

## `anomaly.ue5.build`

- **ID**：`"anomaly.ue5.build"` · **版本** 1 · **capability** `ue5-build`

```c
typedef struct AnomalyUe5BuildServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *build_id)(void* user, char* destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *profile_hash)(void* user, char* destination, size_t* inout_size);
    uint32_t (ANOMALY_CALL *feature_state)(void* user, AnomalyStringViewV1 feature_id);
} AnomalyUe5BuildServiceV1;
```

| 函数 | 说明 |
| --- | --- |
| `build_id` | Build 标识（两段式缓冲）；当前生产 Runtime 已禁用 PE fingerprint，因此返回空字符串 |
| `profile_hash` | 活动 Profile 的 hash（两段式缓冲） |
| `feature_state(feature_id)` | 返回 `AnomalyFeatureStateV1`（0 = 不可用，1 = 可用） |

宿主在 Adapter 启动时发布 `anomaly.ue5.build` 与 `anomaly.nte.build` 供 Feature discovery。请使用 `feature_state` 判断能力是否可用，不要把当前为空的 `build_id` 当作兼容性依据。

## `anomaly.ue5.framework`

- **ID**：`"anomaly.ue5.framework"` · **版本** 1 · **capability** `game-events`

```c
typedef struct AnomalyUe5FrameworkServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    uint32_t (ANOMALY_CALL *game_thread_id)(void* user);
    uint64_t (ANOMALY_CALL *tick_sequence)(void* user);
    int (ANOMALY_CALL *is_game_thread)(void* user);
} AnomalyUe5FrameworkServiceV1;
```

提供游戏线程锚点：`game_thread_id`、单调递增的 `tick_sequence`、`is_game_thread`（当前是否在 Game 线程）。

## `anomaly.ue5.ahud`

- **ID**：`"anomaly.ue5.ahud"` · **版本** 1 · **capability** `ue5-ahud`

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

宿主在 UE 原生 `ReceiveDrawHUD` 完成后，于 Game 线程同步调用订阅者；同一订阅 endpoint 的
callback 串行执行，不会并发进入。`frame`、`frame->user` 及其中全部函数只在当前 callback 返回前
有效；不得缓存，也不得从其他线程调用。服务不向插件暴露 `AHUD*`、`UCanvas*` 或其他 UE 对象。
订阅成功后，宿主会先完成一次实时 UFunction 反射绑定；六个函数的名称、owner、参数类型、偏移和
`ParmsSize` 全部验证通过后才开始 callback admission，绑定仍在扫描期间不会调用订阅者。

| 函数 | 说明 |
| --- | --- |
| `project` | 把三维世界坐标投影到屏幕；成功且位于相机前方时返回非零，`depth` 可为空 |
| `measure_text` | 使用 AHUD 默认字体测量 UTF-8 文本；成功返回非零 |
| `draw_text` | 使用 AHUD 默认字体绘制 UTF-8 文本；成功返回非零 |
| `draw_line` | 绘制指定厚度的线段；成功返回非零 |
| `draw_rect` | 绘制实心矩形；成功返回非零 |
| `subscribe` | 创建独立同步绘制订阅，返回 generation handle |
| `unsubscribe` | 成功后阻止未来 callback admission；从 callback 外调用时，会在返回前排空已进入的 callback |

`color_rgba` 使用 `ANOMALY_RGBA_V1(red, green, blue, alpha)` 的字节布局。回调应只消费已发布的
不可变快照并提交绘制，不应执行文件、网络或生命周期工作。插件异常由宿主边界隔离；同一帧的其他
订阅仍可继续执行。

AHUD 是新插件的世界空间绘制入口；不要在 `anomaly.ui` 的 Render-thread 回调中执行 ESP 投影或绘制。

callback 内允许使用自身 handle 调用 `unsubscribe`。这种 self-unsubscribe 只阻止未来 admission，
不会等待当前 callback 自身返回；插件必须保证 `callback_user` 至少存活到当前 callback 返回。

## `anomaly.ue5.names`

- **ID**：`"anomaly.ue5.names"` · **版本** 1 · **capability** `ue5-names`

```c
typedef struct AnomalyUe5NamesServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *resolve_utf8)(void* user, uint32_t name_id, char* destination, size_t* inout_size);
} AnomalyUe5NamesServiceV1;
```

把 `FName` 的 `name_id` 解码为 UTF-8 字符串（两段式缓冲）。

## `anomaly.ue5.objects`

- **ID**：`"anomaly.ue5.objects"` · **版本** 1 · **capability** `ue5-objects`

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

| 函数 | 说明 |
| --- | --- |
| `generation` | 当前对象表 generation |
| `count` | 对象数量 |
| `snapshot_at(index, ...)` | 按索引取对象快照 |
| `snapshot_by_handle(handle, ...)` | 按 handle 取对象快照 |
| `find_exact(path, ...)` | 仅在 Game 线程按精确 UTF-8 对象路径调用 Profile 验证过的 UE 原生查找；对象尚未加载时返回 `NOT_FOUND` |

`find_exact` 是版本 1 表尾追加字段。调用方必须先用 `struct_size` 检查字段存在；旧宿主不会发布该字段。

辅助宏从 handle 拆分索引 / 序号：

```c
#define ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle)  ((uint32_t)((handle).id) - 1u)
#define ANOMALY_UE5_OBJECT_HANDLE_SERIAL(handle) ((uint32_t)((handle).id >> 32u))
```

## `anomaly.ue5.world`

- **ID**：`"anomaly.ue5.world"` · **版本** 1 · **capability** `ue5-world`

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

`current` 返回当前 World 的 generation handle；`snapshot` 取该 handle 的 World 快照（含 `change_sequence`）。

> [!NOTE]
> 更高层、面向 NTE 玩法的服务（会话事件、玩家 / 相机、实体分页）见 [NTE 服务](nte-services.md)。
