# NTE 服务

对应头文件：`services/nte.h`。这些是**高层 NTE 语义服务**：会话事件、玩家 / 相机、实体 / Actor、角色战斗、技能与快照指标。它们只在活动 [Profile](../user-guide/nte-profiles.md) 的相应符号和运行时反射形状验证通过后发布，否则按 Feature 保持 `UNAVAILABLE`。通用约定见 [API 参考总览](README.md)。

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

没有 `VALID` 或带 `STALE` 的快照不可作为当前状态使用；`PARTIAL` 表示其余字段仍有效但服务明确缺少一部分数据，具体缺失项由各服务合同说明。过期 generation 必须重新取得当前 frame / snapshot。

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

## `anomaly.nte.map-landmarks`

- **ID**：`"anomaly.nte.map-landmarks"` · **版本** 1 · **capability** `nte-map-landmarks`
- **ID 上限**：`ANOMALY_NTE_MAP_LANDMARK_V1_ID_MAX_BYTES` = 128 UTF-8 bytes
- **世界名上限**：`ANOMALY_NTE_MAP_LANDMARK_V1_WORLD_MAX_UTF8_BYTES` = 2048 UTF-8 bytes

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

宿主从活动 Profile 验证的 `TeleportPoint` DataTable 构造目录，只枚举 `CanTeleport` 为真的地标。每条快照包含 DataTable 行的 `TeleportID`、所属世界 `BelongsLevel`、世界坐标、楼层和点类型；`DESTINATION_OVERRIDDEN` 置位时，`destination` 是覆盖坐标，否则与 `world_position` 相同。两个文本数组始终以 NUL 结尾。

目录构建在 Game 线程使用经过 ABI 校验的 `StaticFindObject` 精确解析 `GetGameData` 和 `MapIconTransfer`，不会逐 tick 线性扫描 GObjects。成功目录会被缓存，仅在对象注册表 generation 变化后重建；读取 `sequence()`、`count()` 或 `snapshot_at()` 不触发重新扫描。

目录在同一个非零 `sequence` 内不可变。消费方先读取 `sequence()` 与 `count()`，再按 `[0, count)` 调用 `snapshot_at()`；快照自身的 `sequence` 必须与开始读取时相同。若读取期间序列变化，应丢弃这批结果并重新枚举。`sequence() == 0` 表示目录尚未就绪或 Feature 不可用。

> [!CAUTION]
> `teleport` 是修改调用且仅在 Game 回调域内有效。请求必须回传当前目录的 `sequence` 和索引，过期序列或越界索引返回 `NOT_FOUND`；`flags` 必须为 0，`mode` 只能取上述两个枚举值。宿主在当前目录中重新解析 `TeleportID`，并通过已验证的 `HTPlayerState.MapIconTransfer` / `ProcessEvent` 桥接执行。插件不得跨 ABI 传递 UE 对象指针、原始 DataTable 行地址或自行输入的传送 ID。

该服务依赖 `nte.player`、`ue5.names`、`ue5.objects`、`ue5.object-find` 与 `ue5.process-event`，并受 `nte-map-landmarks-layout-v1` 校验。任一依赖或反射合同不成立时，服务保持 `UNAVAILABLE`。

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

`AnomalyNteActorsServiceV1` 与 `AnomalyNteEntitiesServiceV1` 具有相同函数形状，但用于 **Actor discovery**。某个 World 的首次 `frame` 请求会扫描所有已加载 UWorld level，之后按宿主 actor 采样间隔（`Performance/ActorSnapshotTickInterval`，默认 60 tick）重扫，World 变化时立即重扫；反射读同样仅在 Game 回调域内有效。

重扫是必要的：如果只在 World 变化时扫描，大世界这种全程同一个 World 的场景会让快照永久冻结——已销毁 actor 的条目一直残留，新生成的 actor 永远不可见。

Actor discovery 有意与高频的 Entity 快照分离——前者面向全量枚举，后者面向每帧采样。

---

## `anomaly.nte.combat`

- **ID**：`"anomaly.nte.combat"` · **版本** 1 · **capability** `nte-combat-read`

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

`current_combatant` 返回当前玩家角色的 HP、最大 HP、护盾、死亡状态和攻击目标。`DEAD` 是 combatant 专用低位标志；`VALID / STALE / PARTIAL` 继续使用公共快照高位。目标对象解析失败时目标 handle 为零且快照标记 `PARTIAL`，不会暴露 UObject 地址。

伤害采集使用 `AHTAbilityCharacter::CharacterOnDamaged` 的精确原生广播模板。广播实参直接提供 `FHTDamageEvent`、受击角色、伤害发起角色和 causer；`final_damage` 是 `FHTDamageEvent::Damage` 的整数舍入值，事件带 `ANOMALY_NTE_DAMAGE_V1_CHARACTER_EVENT`。`source_id` 来自同一事件的 `DamageGEDef` 弱对象。客户端展示值和命中位置仍只在相应数据源可用时提供。

暴击状态恢复为原生伤害回调内同步查询：通过已验证的 `K2_GetAbilitySystemComponent` 和 `CurrentDamageIsCrit` 反射绑定，先检查受击者，再检查攻击者，任一返回暴击即标记该次伤害。查询在保留队列槽位之前完成，结果随伤害数据复制入队。所有非空参与者均查询成功且返回 false 时，才标记确认非暴击；查询失败时保留未知状态，已复制的伤害标签仍可作为肯定暴击的备用来源。该路径不执行 FText 名称转换。

`ANOMALY_NTE_DAMAGE_V1_CRITICAL_VALID` 和 `ANOMALY_NTE_COMBAT_EVENT_V1_CRITICAL_VALID` 表示暴击状态已知。只有该位存在时，未设置 `CRITICAL` 才表示确认非暴击；缺少有效位时不得把它计作确认非暴击。新增标志不改变服务表或事件结构布局。

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

`next_damage_event(after_sequence, ...)` 返回第一个更大的保留序列。固定 ring 溢出、World 切换或 Host 重启会使旧非零 cursor 过期；过期和当前没有新事件都返回 `NOT_FOUND`。调用方可比较 `latest_damage_sequence`：若最新序列大于 cursor 但仍取不到下一条，应从 0 重新建立保留窗口游标。

`statistics` 要求当前 `world`，并可按 `source_id`、角色和方向过滤；零 `character` / `source_id` 表示不应用该过滤。统计覆盖当前 World 的宿主保留窗口，ring 曾覆盖或解析曾丢弃时设置 `PARTIAL`，64 位累计饱和时同时设置 `OVERFLOW`，不会整数环绕。`source_name_utf8` 返回当前 combat generation 中缓存的 FName 或精确 `GameplayEffect` 对象路径。`participant_path_utf8` 返回伤害采集时在游戏线程缓存的 attacker/victim UObject 路径；过期 generation 或未参与已采集事件的 handle 返回 `NOT_FOUND`，服务调用本身不会跨线程读取 UObject。

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

`latest_event_sequence` / `next_event` 提供包含伤害、治疗和 Buff 的战斗事件流。`event_name_utf8` 返回事件的显示名称，`participant_display_name_utf8` 返回参与者的显示名称，与 `source_name_utf8` / `participant_path_utf8` 的来源标识和对象路径不同。显示名称在 Game 更新中延后补全；合法请求尚无缓存名称时返回 `NOT_FOUND`。名称缺失不影响伤害事件和数值，消费端应保留记录并显示路径、编号或占位符，随后在 Game 更新中适度重试名称查询。访问追加到 v1 服务表尾部的接口前，应检查 `struct_size` 是否覆盖对应字段。

怪物名称优先匹配具体场景条目和专属文本键，失败后再尝试基础名称。文本键兼容 `_BP` / 场景后缀、编号补零及 `_Name` / `_name` 差异，同时保留变体编号和专属名称的优先级；多个场景条目给出不同基础名称时，不采用该场景基础名称回退。数据表名称字段必须通过 `TextProperty` 类型、大小和偏移校验，图标表不作为名称来源。

FText 解码遵循 [UE5 名称服务](ue5-services.md) 的可选 `ue5.ftext` 校验与只读规则，读取已有显示缓存或已加载文本表的源文字，不调用 `Conv_TextToString`。完整名称查找仍保留角色 ID、怪物静态数据及文本表引用的已验证反射查询，因此不能把整个名称查找流程视为纯内存读取。

消费端应在 Game 域 `on_update` 中查询 combat 服务：先比较 `latest_damage_sequence`，只在序列变化时有界调用 `next_damage_event`，并在接纳新事件时各解析一次 source、attacker 和 victim。HP、统计和解析结果应立即转换为插件自有、可直接绘制的不可变快照。Render 域 `on_draw` 只复制该本地快照并调用 UI 服务，不查询 combat 服务、不推进 cursor，也不解析名称。完整消费边界见 [`examples/nte_combat_demo`](../../examples/nte_combat_demo/plugin.cpp)。

---

## `anomaly.nte.skills`

- **ID**：`"anomaly.nte.skills"` · **版本** 1 · **capability** `nte-skills-read`
- **单页容量上限**：`ANOMALY_NTE_SKILL_PAGE_V1_MAX_CAPACITY` = 128

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

`frame` 固定当前不可变技能帧，`snapshot_at` 和 `page` 只访问 Host cache。分页首请求可使用 generation 0，后续请求必须回传结果 generation；技能重排保持同一组 spec identity 的 handle，技能移除、重新授予、角色 / World 切换或 spec identity 改变会产生新 generation，使旧 handle 返回 `NOT_FOUND`。

skill handle 是宿主生成的 opaque identity，不等于 `FGameplayAbilitySpecHandle`。`ability_class` 同样是 generation handle，可传给 `ability_path_utf8` 和 `ability_display_name_utf8`，不能解释为 UE 地址。v1 已发布 level、input、active / input-pressed / remove 状态；冷却标签形状尚未独立验证，因此当前技能带 `PARTIAL` 且不带 `COOLDOWN_VALID`，两个冷却浮点字段为 0。

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

`ability_display_name_utf8` 返回当前技能类的缓存显示名称，尚未解析时返回 `NOT_FOUND`。名称不可用不表示技能无效；消费端可先显示路径短名或技能编号，并依据当前技能快照及激活服务状态决定是否提供激活操作。该接口同样需要检查服务表的 `struct_size`。

---

## `anomaly.nte.skill-invocation`

- **ID**：`"anomaly.nte.skill-invocation"` · **版本** 1 · **capability** `nte-skill-invocation`

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
> 这是独立的**修改**类服务，只能在 Game 回调域调用。`world`、`character` 和 `skill` 必须来自当前 combat / skills 快照，`flags` 必须为 0。宿主在调用前重新验证当前 ASC、spec handle、ability CDO 与 ability class identity，然后仅通过已验证的 `HTTryActivateAbilityByClass` 入口请求激活；不接受任意路径、UFunction、参数缓冲、目标或位置。

`activate` 返回 `OK` 表示反射桥接调用正常完成，游戏返回的 bool 写入 `accepted`。`accepted == 0` 是游戏拒绝本次请求，不等于 ABI 调用失败；非 Game 线程返回 `CONFLICT`，stale generation 返回 `NOT_FOUND`，且宿主不会自动重试。Render 域 Draw 回调只能把用户意图写入插件自有队列，随后由 Game 域 `on_update` 调用该服务。

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
