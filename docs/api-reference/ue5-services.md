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

## `anomaly.ue5.process-event`

- **ID**：`"anomaly.ue5.process-event"` · **版本** 1 · **capability** `game-events`

```c
typedef void (ANOMALY_CALL *AnomalyUe5ProcessEventCallbackV1)(
    void* user, uintptr_t object, uintptr_t function, void* parameters);
typedef struct AnomalyUe5ProcessEventServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *subscribe)(
        void* user, AnomalyUe5ProcessEventCallbackV1 callback,
        void* callback_user, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *unsubscribe)(
        void* user, AnomalyGenerationHandleV1 handle);
    // Appended V1 extension; check struct_size before reading invoke.
    // Synchronous, Game thread only. Handles come from ue5.objects, not ue5.world.
    // parameters is a caller-owned in/out buffer with capacity >= UFunction::ParmsSize;
    // out/return fields are available on return. Null is valid for zero-parameter calls.
    // The caller supplies an actual UFunction, compatible receiver and initialized UE values.
    // No function allowlist, parameter schema matching, or FunctionFlags rewriting.
    AnomalyStatusV1 (ANOMALY_CALL *invoke)(
        void* user, AnomalyGenerationHandleV1 object,
        AnomalyGenerationHandleV1 function, void* parameters, size_t parameter_size);
} AnomalyUe5ProcessEventServiceV1;
```

| 函数 | 说明 |
| --- | --- |
| `subscribe` | 订阅宿主共享的 UObject ProcessEvent Hook，返回订阅 handle |
| `unsubscribe` | 关闭订阅并排空已进入的回调；订阅 handle 不能用于 `invoke` |
| `invoke` | 在 Game 线程同步调用指定 UFunction，返回后可读取同一缓冲区内的 out/return 字段 |

### 订阅与发布条件

订阅回调在 **Game 线程、原函数之前**串行执行，可以原地修改参数；没有跳过原调用或调用后回调
接口。回调中的对象地址、函数地址和参数只在回调期间有效，不得跨回调缓存。主动调用正常经过
共享 Hook，允许在回调中嵌套调用；订阅者必须避免自行触发的递归循环。

服务在 framework/ProcessEvent Hook 和相应 Profile Feature 可用后发布，不依赖可选 AHUD
反射绑定。服务已可订阅不代表 `invoke` 的对象注册表、调用器和参数大小布局已经可用；缺少这些
前置条件时，`invoke` 返回 `UNAVAILABLE`，不影响已有订阅能力。

### 主动调用合同

本地玩家的 Pawn / Controller 可通过 [`nte.player.get_object_handles`](nte-services.md#玩家对象句柄)
取得兼容对象句柄。`player.snapshot.handle` 是玩家样本身份，不能直接作为接收对象传入。

| 参数 | 合同 |
| --- | --- |
| `user` | 原样回传服务表中的 `user` |
| `object` | 接收调用的对象 handle，必须来自 `anomaly.ue5.objects` |
| `function` | UFunction 的对象 handle，同样来自 `anomaly.ue5.objects` |
| `parameters` | 调用者拥有的可写 UE 参数缓冲区；零参数函数允许 `NULL` |
| `parameter_size` | 缓冲区容量，至少为 Profile 布局中该 UFunction 的 `ParmsSize`，可以更大 |

对象／函数 handle 可由 `snapshot_at`、`snapshot_by_handle` 或 `find_exact` 获得；不能传入
`ue5.world` 的独立 handle、订阅 handle，或把裸地址强制填进 handle。插件不需要自己扫描
ProcessEvent 入口。调用仅允许在 Game 线程执行，例如插件 Game update 或 ProcessEvent
回调，不能直接在 UI Render 回调、Lifecycle 或 Worker 线程调用；宿主不会自动排队或跨线程执行。

校验只覆盖服务与调用器/注册表可用、线程、handle 的 generation/serial、可读的 `ParmsSize`
和缓冲区容量。不设函数白名单，不做 owner 或逐字段 schema 匹配，不修改 `FunctionFlags`；
也没有额外的 4 KiB 缓冲区上限或要求 UFunction 地址高于 4 GiB 的限制。

这是低层调用边界：调用者须保证 `function` 确实是 UFunction、receiver 与函数兼容，并正确
构造/销毁参数中的 UE 类型（如字符串、数组）。原生和蓝图函数均沿引擎正常分派，不绕过引擎条件。
缓冲区在调用期间须保持有效，宿主不复制或保留它；返回值在已知布局的 return 字段内，没有独立
返回值参数。`OK` 只表示分派完成，不代表函数内部业务成功。异常隔离不保证撤销已发生的引擎副作用。

### 兼容调用示例

`invoke` 是 V1 表尾扩展，服务版本仍为 1，原有字段偏移不变。使用
`Host::Query<AnomalyUe5ProcessEventServiceV1>(ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID, 1)`
或 C Host API 查询服务后，仍须检查表大小再读取 `invoke`，以兼容仅支持订阅的旧宿主。
以下 helper 使用已经取得的对象／函数 handle，**必须在 Game 线程调用**：

```c
AnomalyStatusV1 InvokeUFunction(
    const AnomalyUe5ProcessEventServiceV1* service,
    AnomalyGenerationHandleV1 object,
    AnomalyGenerationHandleV1 function,
    void* parameters, size_t capacity) {
    if (service == NULL ||
        service->struct_size < offsetof(AnomalyUe5ProcessEventServiceV1, invoke) +
            sizeof(service->invoke) || service->invoke == NULL) {
        AnomalyStatusV1 unavailable = {
            ANOMALY_STATUS_V1_UNAVAILABLE, 0, {NULL, 0}};
        return unavailable;
    }
    return service->invoke(service->user, object, function, parameters, capacity);
}
```

### `invoke` 状态码

| 状态 | 含义 |
| --- | --- |
| `OK` | 引擎分派已返回，out/return 字段可由调用者读取 |
| `CONFLICT` | Game 线程尚未确定，或当前不是 Game 线程 |
| `NOT_FOUND` | 对象／函数 handle 失效，或参数大小字段不可读 |
| `INVALID_ARGUMENT` | 非零参数函数传入空缓冲区 |
| `BUFFER_TOO_SMALL` | 缓冲区容量小于 `ParmsSize` |
| `UNAVAILABLE` | 服务已停止，或所需调用器、对象注册表、Feature/布局不可用 |
| `FAILED` | 调用器报告失败，或调用边界捕获异常 |

停止后缓存的服务表只返回 `UNAVAILABLE`，不会在下一次 Start 恢复；须重新查询当前 generation
的服务并重新取得有效对象 handle。

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
    AnomalyStatusV1 (ANOMALY_CALL *resolve_ftext_utf8)(
        void* user, uintptr_t ftext_address, char* destination, size_t* inout_size);
} AnomalyUe5NamesServiceV1;
```

把 `FName` 的 `name_id` 解码为 UTF-8 字符串（两段式缓冲）。

`resolve_ftext_utf8` 接受当前 Game 快照中有效的 `FText` 地址，需要可选的 `ue5.ftext` 布局校验通过。它优先复制已有的本地化显示文字；对于文本表引用，尚无显示缓存时读取已加载文本表的源文字，源文字不保证与当前语言的翻译一致。该操作不调用 UE 文本函数、不加载资源、不重建文本历史，也不保留输入地址。未知类型、无效字符串或未加载的文本表返回 `NOT_FOUND`；普通 `FName` 解析独立可用。

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

---

## `anomaly.ue5.streaming-source`

- **ID**：`"anomaly.ue5.streaming-source"` · **版本** 1 · **capability** `ue5-streaming-source`
- **依赖**：feature `ue5.streaming-source`（依赖 `nte.player`，validator `ue5-streaming-source-layout-v1`，layout 键 `controller.streamingSourceVtableOffset`）

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

`set_override` 让局部玩家控制器的流式源在该位置求值，从而在角色移动过去之前把目标区域加载出来；`rotation` 仅在 `ANOMALY_UE5_STREAMING_SOURCE_OVERRIDE_V1_ROTATION` 置位时生效（自由相机跟随视角用）。`duration_milliseconds` 为 `UNTIL_CLEARED` 时一直保留到 `clear_override`，其他值在该毫秒数后自动失效——即使消费者忘记清除也不会长期劫持流式源。两个调用都必须在 Game 回调域内执行，否则返回 `CONFLICT`。

> [!NOTE]
> 宿主拥有**唯一的**进程级流式源 hook，因此多个插件可以同时消费该服务，但共享同一个覆盖槽位：最后一次请求生效，直到它过期或被清除。唯一的例外是**传送到达窗口**——宿主会为该次传送保留槽位，此时其他消费者的 `set_override` 返回 `CONFLICT`（窗口结束后即可再次申请），以免传送落点的流式源被改写到别处。消费者应在自己不再需要时调用 `clear_override`（例如插件停止、相机停止飞行）。该服务只在其 layout 键、validator 与 `nte.player` 依赖同时通过时才发布；不满足时调用返回 `UNAVAILABLE`，调用方应降级为直接操作（例如同步传送）。`anomaly.nte.player-teleport` 的默认预载模式也构建在该服务之上。

> [!NOTE]
> 更高层、面向 NTE 玩法的服务（会话事件、玩家 / 相机、实体分页）见 [NTE 服务](nte-services.md)。

## `anomaly.ue5.assets`

- **ID**：`"anomaly.ue5.assets"` · **版本** 1 · **capability** `ue5-assets`

通过 Asset Registry 的派生类索引列出完整蓝图类路径，包含从未加载的类。
另可主动异步加载指定的 StringTable 资产并取得整表源文本；不依赖场景里已加载的角色，
也不会为了收集名称批量加载怪物蓝图或 CDO。名称表源文本不等于按语言选取的 `locres` 翻译。

| 成员 | 线程 | 说明 |
| --- | --- | --- |
| `prepare_class` | Game | 绑定已加载基类的完整路径，如 `/Script/HTGame.HTAICharacter` |
| `enumerate_derived` | Worker | 查询注册表，通过同步 visitor 返回完整生成类路径 |
| `prepare_string_table` | Game | 请求名称表异步加载，后续每次最多复制 8 项，并在条目之间检查 1 ms 软预算 |
| `enumerate_string_table` | Worker | 通过同步 visitor 读取完整、不可变的键 / 源文本快照 |

键和源文本都允许为空，空条目不会中止采集或枚举。

准备方法返回 `UNAVAILABLE` 表示尚未就绪，调用方应在后续 Game tick 继续，不能循环等待。
`prepare_string_table` 只接受调用方已确认是 StringTable 的资产路径。原生加载收尾和单次引擎调用
无法抢占，因此软预算不保证每次调用的绝对耗时。长目录查询、排序、名称匹配和文件 I/O 应在 Worker。
服务依赖活动 Profile 的 `ue5.assets` ABI 校验、注册表入口签名、内存释放入口及反射参数合同，
不匹配时不可用，不猜测偏移。

visitor 在调用者的 Worker 中同步执行，不会被宿主保留；字符串视图只在当前 visitor 调用期间有效。
返回 0 会取消枚举。调用方只应在方法返回 `OK` 后发布新目录；任何失败或取消都应丢弃本次部分结果。
路径已经包含生成类名称，不得额外添加 `_C`。不同基类查询互不覆盖，没有共享分页游标。
停止服务会撤销并排空调用 lease；旧服务表在停止后返回 `UNAVAILABLE`。

插件可用 `anomaly.scheduler` 安排 Worker，并用 `anomaly.storage` 原子保存用户缓存。
缓存只用于启动加速，每次启动应重新查询注册表；成功后整体替换目录，以移除游戏更新后消失的资产。
失败时保留旧快照，并显示缓存 / 刷新失败状态。SummonLab 的 `assets/catalog-policy.json` 只保存名称表路径、
必要的名称键关联和玩法策略，不保存导出的蓝图清单或中文文本。
