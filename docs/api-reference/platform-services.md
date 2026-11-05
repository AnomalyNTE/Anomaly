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

## `anomaly.json`

- **ID**：`"anomaly.json"` · **版本** 1 · **capability** `json`

为插件提供作用域内的轻量 JSON DOM 解析 / 遍历 / 序列化。所有返回的 handle 都绑定
plugin generation，插件停止时由 Scope 统一失效；插件也可以在不再使用时主动 `release`。

```c
typedef enum AnomalyJsonKindV1 {
    ANOMALY_JSON_V1_NULL = 0, ANOMALY_JSON_V1_BOOLEAN = 1,
    ANOMALY_JSON_V1_NUMBER = 2, ANOMALY_JSON_V1_STRING = 3,
    ANOMALY_JSON_V1_ARRAY = 4, ANOMALY_JSON_V1_OBJECT = 5
} AnomalyJsonKindV1;

typedef struct AnomalyJsonServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *parse)(void* user, AnomalyStringViewV1 document,
        AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *release)(void* user, AnomalyGenerationHandleV1 handle);
    AnomalyStatusV1 (ANOMALY_CALL *kind)(void* user, AnomalyGenerationHandleV1 handle,
        uint32_t* kind);
    AnomalyStatusV1 (ANOMALY_CALL *boolean_value)(void* user,
        AnomalyGenerationHandleV1 handle, int32_t* value);
    AnomalyStatusV1 (ANOMALY_CALL *number_value)(void* user,
        AnomalyGenerationHandleV1 handle, double* value);
    AnomalyStatusV1 (ANOMALY_CALL *string_value)(void* user,
        AnomalyGenerationHandleV1 handle, char* destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *array_size)(void* user,
        AnomalyGenerationHandleV1 handle, size_t* size);
    AnomalyStatusV1 (ANOMALY_CALL *array_item)(void* user,
        AnomalyGenerationHandleV1 handle, size_t index, AnomalyGenerationHandleV1* child);
    AnomalyStatusV1 (ANOMALY_CALL *object_size)(void* user,
        AnomalyGenerationHandleV1 handle, size_t* size);
    AnomalyStatusV1 (ANOMALY_CALL *object_key_at)(void* user,
        AnomalyGenerationHandleV1 handle, size_t index, char* destination,
        size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *object_find)(void* user,
        AnomalyGenerationHandleV1 handle, AnomalyStringViewV1 key,
        AnomalyGenerationHandleV1* child);
    AnomalyStatusV1 (ANOMALY_CALL *serialize)(void* user,
        AnomalyGenerationHandleV1 handle, char* destination, size_t* inout_size);
} AnomalyJsonServiceV1;
```

`parse` 接受完整 UTF-8 JSON 文档；`string_value` / `serialize` / `object_key_at` 遵循
两段式缓冲协议。`array_item` 和 `object_find` 返回的 child handle 也必须释放，或随
plugin generation 一起失效。

---

## `anomaly.profile`

- **ID**：`"anomaly.profile"` · **版本** 1 · **capability** `json`
- 头文件：`anomaly/sdk/services/profile.h`；C++ 辅助：`anomaly/sdk/profile.hpp`。
- manifest 同时声明 `anomaly.profile` 和 `anomaly.json` v1。

```c
typedef struct AnomalyProfileServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *load)(void* user,
        AnomalyStringViewV1 relative_path, AnomalyGenerationHandleV1* document);
} AnomalyProfileServiceV1;
```

`load("my-plugin.profile.json")` 从插件当前 DLL generation 的包快照中读取
`profile/my-plugin.profile.json`，不访问用户 state/config 目录。仅 Lifecycle/Worker 可调用；
Game/Render 返回 `UNAVAILABLE`。输入必须是 UTF-8 相对 `.json` 路径，最多 512 字节；拒绝绝对路径、
父目录遍历、reparse 和超过 1 MiB 的文件。缺失文件返回 `NOT_FOUND`，路径、格式或 schema
错误返回 `INVALID_ARGUMENT`，I/O 故障返回 `FAILED`；失败时输出句柄为零。

返回值是只读 `anomaly.json` 文档句柄，使用该服务查询并 `release`。每个 child handle 独立持有
文档，释放 root 不影响尚存的 child；所有句柄均受 generation 和 PluginScope 管理。C++
`LoadProfile` 返回包含字符串、布局和候选的自有 `Profile` 快照，返回前释放临时宿主句柄，
无需插件依赖第三方 JSON 库。`Profile::symbol(name)` 对未声明名称抛出 `std::out_of_range`，
插件应在安装 Hook 前检查自身需要的字段。

同一公开头文件还提供插件侧 C++ 辅助接口，随独立 SDK 安装，不改变 C ABI：

- `ProfileContract{game, module, section, contract}` 与 `LoadProfile(host, filename, expected)`：
  校验调用方声明的语义合同；`Profile::Matches` 可校验已有快照。
- `Profile::Value(key, maximum, alignment=1)`：检查必需字段、上界及对齐，失败抛出异常，不能跨 DLL 回调边界传播。
- `MatchesProfileEntry(core, profile, symbol, address, observer_detour=false)`：在受控地址校验入口。
  最后一个参数仅用于宿主五字节 E9 observer detour，启用后仍必须匹配剩余入口字节。
- `ProfileScanCache::Prepare(host, profile, symbols, prefer_batch=true)` / `Resolve(core, profile, name)`：
  同一 Profile 的指定符号共用去重扫描结果；`Resolve` 仍校验唯一候选、调用目标及入口。仅在本次初始化内使用缓存。
- `ProfileSnapshot::Load(api, filename, expected, known_entries={}, batch_signatures=false, layout_symbol={})`：
  加载与解析后发布插件自有快照，提供 `Address`、`Selected`、`Pattern`、`Value`、`MatchesEntry`、`Document`。
  指定的布局符号必须唯一解析成功；其他未解析符号地址为零，由插件停用依赖功能。失败不替换旧快照，
  调用方必须检查返回值；成功加载、赋值或销毁会使旧的借用引用失效。初始化和读取不得并发。

这些接口没有 NTE 默认条件，布局候选符号由插件显式指定。原有 `LoadProfile` 和
`ResolveProfileSymbol` 保持可用。详见[插件开发中的 Profile 示例](../developer-guide/plugin-development.md)。

所有插件共用仓库 `schemas/plugin-profile.schema.json`（SDK 安装到 `share/anomaly/schemas/`）。
`schemaVersion` 固定为 1，不接受未知根字段、重复键或重复符号名：

| 字段 | 内容 |
| --- | --- |
| `game` / `module` / `section` | 游戏标识、模块名与 PE 节名 |
| `contract` | 插件实现的语义合同；不包含或匹配 Build identity |
| `layout` | 名称到 uint32 的共享偏移、尺寸与种子 |
| `symbols` | 0–128 个 `{name, candidates}`，每个符号 1–16 个候选 |
| 候选 `pattern` / `entry` | 空格分隔的十六进制字节；仅 pattern 允许 `??`，entry 必须精确 |
| 候选 `callOffset` | 可选 E8 rel32 指令位置，必须落在 pattern 范围内 |
| 候选 `values` | 该候选特有的 uint32 布局数据 |

C++ `ResolveProfileSymbol(signature, core, profile, name)` 使用现有 signature/memory 服务，
检查唯一候选、可选 E8 rel32 目标与精确入口字节；失败返回空结果和 `error`，不安装 Hook。
成功结果的 `candidate` 借用输入 profile，使用期间须保持该快照存活。该 helper 还需
`interop-signature` 和 `memory-read` capability。Schema 校验不等于布局已适配，插件仍须
校验语义合同、所需字段和调用协议；不能用编译期签名或偏移作为失败兜底。

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
