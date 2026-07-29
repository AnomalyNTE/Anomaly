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
} AnomalyUe5ObjectsServiceV1;
```

| 函数 | 说明 |
| --- | --- |
| `generation` | 当前对象表 generation |
| `count` | 对象数量 |
| `snapshot_at(index, ...)` | 按索引取对象快照 |
| `snapshot_by_handle(handle, ...)` | 按 handle 取对象快照 |

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
