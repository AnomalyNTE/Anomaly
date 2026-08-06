# NTE 服务

对应头文件：`services/nte.h`。这些是**高层 NTE 语义服务**：会话事件、玩家 / 相机、实体 / Actor 分页与快照指标。它们只在活动 [Profile](../user-guide/nte-profiles.md) 的相应符号验证通过后发布，否则按 Feature 保持 `UNAVAILABLE`。通用约定见 [API 参考总览](README.md)。

> [!IMPORTANT]
> 服务表属于一个 Host 生命周期 generation。来自已停止 / 已替换 generation 的缓存表只报告 `UNAVAILABLE`（标量查询返回 0），且非零 cursor / generation 不跨 Host 重启存活。

## 快照有效性标志

许多 NTE 快照的 `flags` 字段使用：

```c
typedef uint32_t AnomalyNteSnapshotFlagsV1;
#define ANOMALY_NTE_SNAPSHOT_V1_INVALID 0u
#define ANOMALY_NTE_SNAPSHOT_V1_VALID   (1u << 29u)
#define ANOMALY_NTE_SNAPSHOT_V1_STALE   (1u << 30u)
#define ANOMALY_NTE_SNAPSHOT_V1_PARTIAL (1u << 31u)
```

`VALID / STALE / PARTIAL` 与过期 generation 不可用于普通绘制。

## `anomaly.nte.pickup`

- **ID**：`"anomaly.nte.pickup"` · **版本** 1 · **capability** `nte-pickup`

该服务把周围拾取和拾取确认固化在 Host 的 NTE Adapter 中。插件只提交半径与单次上限，
不接触 UE 对象、UFunction、ProcessEvent、内存地址或 Profile 偏移。

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

`request_nearby` 只允许 Host 的 Game callback domain。返回 `OK` 表示请求已排队；实际候选
扫描和交互发生在后续 Game tick，`snapshot` 可从 Render/UI 域读取。`radius` 合同为 50--5000，
`maximum_items` 为 1--128，`flags` 与 `reserved` 必须为 0。
服务可用且尚未提交请求时，快照为 `VALID + IDLE + OK`。

`nearby` 是半径内的 `HTRandomItemActor`、`PropBox_` 或 `InteractBox_` 数量；`triggered`
是成功调用 `TriggerInteract` 的数量；`confirmed` 由后续完整实体快照中的对象消失、
`bInteractFinish` 变化，或截止时一次 `BPCanTryInteract == false` 得到；`checking` 是仍在确认
窗口内的数量；`unconfirmed` 是窗口结束后仍可交互或最终状态不可读的数量；`skipped` 汇总
不可读、不可拾取或未通过 `BPCanTryInteract` 的候选。

确认路径只读取现有实体缓存和一个状态字节，最短间隔 100 ms；窗口内不重复调用反射函数。
最多 2 秒后才执行一次最终 `BPCanTryInteract`。超时保持 `OK + unconfirmed`，不会把已触发
请求改写为 `FAILED`，也不会让快照永久停留在 `CHECKING`。

快照 `flags` 使用 `VALID`、`CHECKING_FLAG` 与 `HAS_UNCONFIRMED`。`sequence` 每次请求递增，
World、对象注册表或 Host generation 变化会清理当前请求并返回 `UNAVAILABLE`。服务只有在
活动 Profile 的 `nte-pickup-layout-v1`、UE5 ProcessEvent ABI、对象/名称/玩家/实体依赖均
验证通过后才发布。`TriggerInteract`、`BPCanTryInteract`、`BPGetInteractEntries` 的
`numParms`、`parmsSize`、全部参数偏移和 entry 布局都来自活动 Profile；Host 不使用插件侧
常量补全这些布局。

---

## `anomaly.nte.build`

- **ID**：`"anomaly.nte.build"` · **版本** 1 · **capability** `nte-build`

```c
typedef struct AnomalyNteBuildServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *build_id)(void* user, char* destination, size_t* inout_size);
    uint32_t (ANOMALY_CALL *feature_state)(void* user, AnomalyStringViewV1 feature_id);
} AnomalyNteBuildServiceV1;
```

`feature_state` 返回 `AnomalyFeatureStateV1`，是 NTE Feature Matrix 的查询入口。当前生产 Runtime 已禁用 PE fingerprint，因此 `build_id` 返回空字符串；不要用它判断 Feature 是否可用。

---

## `anomaly.nte.esc-menu-button`

- **ID**：`"anomaly.nte.esc-menu-button"` · **版本** 1 · **capability** `nte-esc-menu-button`

该服务只扩展 **NTE 的 ESC 菜单**，不是通用菜单或通用 UI 服务。插件注册按钮后，NTE 桥接层按注册顺序把它追加到当前 ESC 菜单已有按钮的末尾；已有按钮数量不属于 ABI 合同，调用方不能假定固定为 25 个。

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

`flags` 必须为 `ANOMALY_NTE_ESC_MENU_BUTTON_V1_NONE`，`reserved` 必须为 0。`id` 在同一插件 owner 内必须唯一；不同插件可以使用相同 ID。`icon_format` 可为 `NONE` 或 `PNG`；`PNG` 的 `icon_bytes` 上限为 1 MiB，`NONE` 要求空字节 span。宿主复制 `id`、UTF-8 `label` 与图标字节，因此注册返回后原始内存可立即释放。成功注册得到的 generation handle 只属于该插件 generation；显式 `unregister_button` 或插件 Scope 撤销都会使其失效，并阻止 callback 越过 generation 生命周期。

用户点击按钮后，宿主在 NTE game thread 上调用注册时提供的 callback，插件可在 callback 中实现自己的按钮事件。callback 返回 `NONE` 时不触发额外宿主动作；返回 `EXPAND_ANOMALY` 时请求展开 Anomaly 管理界面。自定义 PNG 在同一次菜单构建中连续导入失败 3 次后，该按钮使用 NTE 按钮 widget 的默认图标继续追加；下一次菜单重建会重新尝试自定义图标。Anomaly 自带的默认按钮使用内嵌 `logo.png`，并作为宿主追加区的第一项注册；第三方插件按注册成功顺序继续向后追加。

---

## `anomaly.nte.session`

- **ID**：`"anomaly.nte.session"` · **版本** 1 · **capability** `nte-session-snapshot`

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

事件流永不暴露 World 指针，调用方只保留 opaque、单调递增的 cursor 与 generation handle。`next_event(after_sequence, ...)` 返回 sequence 大于 `after_sequence` 的第一个保留事件；stale 的非零 cursor 与空的未来区间都返回 `NOT_FOUND`。

---

## `anomaly.nte.player`

- **ID**：`"anomaly.nte.player"` · **版本** 1 · **capability** `nte-player-snapshot`

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
} AnomalyNtePlayerServiceV1;
```

相机数据只在活动 Profile 验证了 Player 服务的可选 `nte.player-esp` capability 后可用。`world` 标识场景，`player` 标识提供该相机样本的 Pawn / Controller；任一 generation handle 变 stale 会使对应关系失效。

---

## `anomaly.nte.player-teleport`

- **ID**：`"anomaly.nte.player-teleport"` · **版本** 1 · **capability** `nte-player-teleport`

```c
typedef struct AnomalyNtePlayerTeleportRequestV1 {
    uint32_t struct_size; uint32_t flags;
    AnomalyGenerationHandleV1 world; AnomalyGenerationHandleV1 player;
    double position[3];
} AnomalyNtePlayerTeleportRequestV1;
typedef struct AnomalyNtePlayerTeleportServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *teleport)(void* user,
        const AnomalyNtePlayerTeleportRequestV1* request);
} AnomalyNtePlayerTeleportServiceV1;
```

> [!CAUTION]
> 这是**修改**类服务。`teleport` 仅在 Game 回调域内有效；若 UE 拒绝请求或调用后位置检查未到达目标，返回 `FAILED`。宿主提供 `bSweep=false`、`bTeleport=true`，不暴露 UE 对象指针或 `FHitResult` ABI。`world` 与 `player` 必须来自当前快照，stale handle 会被拒绝。该服务只在其引擎 `ProcessEvent` 签名、ABI / 反射、依赖与 Game-thread gate 同时通过时才发布；**Pawn-vtable fallback 被禁止**。

---

## `anomaly.nte.navigation`

- **ID**：`"anomaly.nte.navigation"` · **版本** 1 · **capability** `nte-navigation`

```c
typedef struct AnomalyNteNavigationServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *move_to_location)(
        void* user, const double destination[3]);
    AnomalyStatusV1 (ANOMALY_CALL *stop_movement)(void* user);
} AnomalyNteNavigationServiceV1;
```

> [!CAUTION]
> 这是**修改**类服务，两个调用仅在 Game 回调域内有效。`move_to_location` 使用当前本地 Controller 的 `ControlRotation` 调用原生 `HTUtil.MoveToPointByTransform`，固定传入 `ForceWalk=false`、`AutoControl=true`、`bHideUI=false`、`ProtectTime=0` 与 `bUsingPathFinding=true`。宿主仅在该调用期间改写 Patrol 输入捕获，使角色保留原生转向、移动和动作，同时不锁定鼠标或 UI。`stop_movement` 调用当前 Controller 的原生 `StopMovement`。宿主不暴露 UE 对象指针；服务只有在 ProcessEvent、反射布局、输入函数 ABI 与所有依赖同时验证后才发布。

---

## `anomaly.nte.entities`

- **ID**：`"anomaly.nte.entities"` · **版本** 1 · **capability** `nte-entity-snapshot`
- **单页容量上限**：`ANOMALY_NTE_ENTITY_PAGE_V1_MAX_CAPACITY` = 256

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
    uint32_t struct_size; uint32_t flags;      // flags 保留，须为 0
    uint64_t generation;                        // 0 选当前缓存帧
    uint32_t offset; uint32_t capacity;         // capacity ≤ 256
    uint64_t class_id;
    uint32_t class_name_id; uint32_t entity_name_id;
    uint32_t required_flags; uint32_t excluded_flags;  // 对 AnomalyNteEntityFlagsV1 求值
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

`frame` 取当前不可变实体帧（含相机与 `generation`）；名称解析只读当前帧缓存，不在调用时读取实时游戏内存。分页的 `generation` 为 0 时选择当前帧，后续页必须传回返回的 generation；stale 的非零 generation 返回 `NOT_FOUND`。三个有界反射读仅在 Game 回调域内有效，不暴露 UE 对象地址，并在读取前校验字段形状。

---

## `anomaly.nte.actors`

- **ID**：`"anomaly.nte.actors"` · **版本** 1 · **capability** `nte-actor-snapshot`

`AnomalyNteActorsServiceV1` 与 `AnomalyNteEntitiesServiceV1` 具有相同函数形状，但用于 **Actor discovery**。某个 World 的首次 `frame` 请求会扫描所有已加载 UWorld level 并为该 World 缓存结果；反射读同样仅在 Game 回调域内有效。

Actor discovery 有意与高频的 Entity 快照分离——前者面向全量枚举，后者面向每帧采样。

---

## `anomaly.nte.metrics`

- **ID**：`"anomaly.nte.metrics"` · **版本** 1 · **capability** `nte-snapshot-metrics`

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

`snapshot` 报告**宿主**的采样工作指标（快照耗时、玩家 / 实体刷新与缓存命中计数等），用于诊断而非逐插件遍历。用法见 [`examples/nte_inspector`](../../examples/nte_inspector/plugin.cpp)。
