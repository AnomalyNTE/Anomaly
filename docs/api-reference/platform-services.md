# 平台作用域服务

对应头文件：`services/platform.h`。这些服务是彼此独立版本化的作用域服务表，各自需要对应 capability。通用约定见 [API 参考总览](README.md)。

## 通用类型

```c
typedef enum AnomalyNotificationSeverityV1 {
    ANOMALY_NOTIFICATION_V1_INFO = 0, ANOMALY_NOTIFICATION_V1_WARNING = 1,
    ANOMALY_NOTIFICATION_V1_ERROR = 2
} AnomalyNotificationSeverityV1;

typedef struct AnomalyRuntimeInfoV1 {
    uint32_t struct_size;
    uint32_t runtime_version_major, runtime_version_minor, runtime_version_patch;
    uint32_t process_id, thread_id;
    uint64_t uptime_milliseconds, plugin_generation;
} AnomalyRuntimeInfoV1;
```

回调 typedef：

```c
typedef AnomalyStatusV1 (ANOMALY_CALL *AnomalyConfigMigrationV1)(void* user,
    uint32_t source_schema_version, AnomalyByteSpanV1 source,
    AnomalyMutableByteSpanV1 destination, size_t* inout_size);
typedef AnomalyStatusV1 (ANOMALY_CALL *AnomalyDiagnosticSelfTestV1)(void* user,
    AnomalyMutableByteSpanV1 destination, size_t* inout_size);
typedef void (ANOMALY_CALL *AnomalyTaskCallbackV1)(void* user, AnomalyGenerationHandleV1 task);
typedef AnomalyStatusV1 (ANOMALY_CALL *AnomalyCommandCallbackV1)(void* user,
    AnomalyStringViewV1 arguments, AnomalyMutableByteSpanV1 destination, size_t* inout_size);
```

---

## `anomaly.config`

- **ID**：`"anomaly.config"` · **版本** 1 · **capability** `configuration`

管理**插件本地**的 JSON 文档：注册 schema，然后读 / 原子写 / 迁移。宿主拥有持久化位置（`Anomaly\config\plugins\`）。

```c
typedef struct AnomalyConfigServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *register_schema)(void* user,
        AnomalyStringViewV1 schema_id, uint32_t schema_version,
        AnomalyByteSpanV1 schema_json, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *unregister_schema)(void* user, AnomalyGenerationHandleV1 handle);
    AnomalyStatusV1 (ANOMALY_CALL *read)(void* user, AnomalyStringViewV1 schema_id,
        uint32_t* schema_version, AnomalyMutableByteSpanV1 destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *write_atomic)(void* user, AnomalyStringViewV1 schema_id,
        uint32_t schema_version, AnomalyByteSpanV1 document);
    AnomalyStatusV1 (ANOMALY_CALL *migrate)(void* user, AnomalyStringViewV1 schema_id,
        AnomalyConfigMigrationV1 migration, void* migration_user);
} AnomalyConfigServiceV1;
```

| 函数 | 说明 |
| --- | --- |
| `register_schema` | 在**每个已加载 generation** 中注册稳定的 `schema_id`，之后才能 read / write / migrate；返回的 handle 由 Scope 拥有 |
| `unregister_schema` | 注销 schema handle |
| `read` | 两段式缓冲读取持久文档；`NOT_FOUND` 表示尚无保存；`schema_version` 回传存储版本 |
| `write_atomic` | 对注册 schema 校验后**原子替换**文档 |
| `migrate` | 用回调把旧版本文档迁移到当前 schema |

用法见 [`examples/reliable_config`](../../examples/reliable_config/plugin.cpp)。

---

## `anomaly.storage`

- **ID**：`"anomaly.storage"` · **版本** 1 · **capability** `storage`

插件私有的原始文件读 / 原子写 / 删除（相对路径）。

```c
typedef struct AnomalyStorageServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *read)(void* user, AnomalyStringViewV1 relative_path,
        AnomalyMutableByteSpanV1 destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *write_atomic)(void* user,
        AnomalyStringViewV1 relative_path, AnomalyByteSpanV1 source);
    AnomalyStatusV1 (ANOMALY_CALL *remove)(void* user, AnomalyStringViewV1 relative_path);
} AnomalyStorageServiceV1;
```

结构化设置优先用 `anomaly.config`；`storage` 适合二进制或非 schema 数据。

---

## `anomaly.runtime-info`

- **ID**：`"anomaly.runtime-info"` · **版本** 1 · **capability** `runtime-info`

```c
typedef struct AnomalyRuntimeInfoServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *snapshot)(void* user, AnomalyRuntimeInfoV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *runtime_version_utf8)(void* user, char* destination, size_t* inout_size);
} AnomalyRuntimeInfoServiceV1;
```

`snapshot` 填 `AnomalyRuntimeInfoV1`（版本、PID、TID、uptime、plugin generation）；`runtime_version_utf8` 返回版本字符串（两段式缓冲）。

---

## `anomaly.diagnostics`

- **ID**：`"anomaly.diagnostics"` · **版本** 1 · **capability** `diagnostics`

注册自检、运行自检、导出诊断快照。

```c
typedef struct AnomalyDiagnosticsServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *register_self_test)(void* user, AnomalyStringViewV1 id,
        AnomalyDiagnosticSelfTestV1 callback, void* callback_user, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *unregister_self_test)(void* user, AnomalyGenerationHandleV1 handle);
    AnomalyStatusV1 (ANOMALY_CALL *run_self_test)(void* user, AnomalyStringViewV1 id,
        AnomalyMutableByteSpanV1 destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *snapshot_json)(void* user,
        AnomalyMutableByteSpanV1 destination, size_t* inout_size);
} AnomalyDiagnosticsServiceV1;
```

> [!NOTE]
> 这与[诊断命名管道协议](../user-guide/diagnostics-cli.md)是不同层：此处是插件向宿主注册自检，管道协议是外部客户端向 Runtime 发命令。

---

## `anomaly.scheduler`

- **ID**：`"anomaly.scheduler"` · **版本** 1 · **capability** `scheduler`

```c
typedef struct AnomalySchedulerServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *schedule)(void* user, uint32_t delay_milliseconds,
        AnomalyTaskCallbackV1 callback, void* callback_user, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *cancel)(void* user, AnomalyGenerationHandleV1 handle);
} AnomalySchedulerServiceV1;
```

`schedule` 在 `delay_milliseconds` 后回调；返回的 handle 绑定 generation，未开始的任务在 stop / reload 时自动取消，也可用 `cancel` 主动取消。

---

## `anomaly.commands`

- **ID**：`"anomaly.commands"` · **版本** 1 · **capability** `commands`

```c
typedef struct AnomalyCommandsServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *register_command)(void* user, AnomalyStringViewV1 name,
        AnomalyStringViewV1 description, AnomalyCommandCallbackV1 callback,
        void* callback_user, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *unregister_command)(void* user, AnomalyGenerationHandleV1 handle);
    AnomalyStatusV1 (ANOMALY_CALL *invoke)(void* user, AnomalyStringViewV1 name,
        AnomalyStringViewV1 arguments, AnomalyMutableByteSpanV1 destination, size_t* inout_size);
} AnomalyCommandsServiceV1;
```

注册命名命令，命令回调接收 `arguments` 并写入 `destination`（两段式缓冲）；`invoke` 调用已注册命令。

---

## `anomaly.notifications`

- **ID**：`"anomaly.notifications"` · **版本** 1 · **capability** `notifications`

```c
typedef struct AnomalyNotificationsServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *post)(void* user, AnomalyNotificationSeverityV1 severity,
        AnomalyStringViewV1 title, AnomalyStringViewV1 body,
        uint32_t timeout_milliseconds, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *dismiss)(void* user, AnomalyGenerationHandleV1 handle);
} AnomalyNotificationsServiceV1;
```

`post` 发布通知（severity + 标题 + 正文 + 超时），返回 handle 可用 `dismiss` 主动关闭。
