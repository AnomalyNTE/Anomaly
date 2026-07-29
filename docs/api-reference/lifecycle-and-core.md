# 生命周期与 Core 服务

对应头文件：`plugin.h`、`services/core.h`、`services/plugin_state.h`、`services/localization.h`。通用约定见 [API 参考总览](README.md)。

## 插件入口

插件 DLL 导出一个名为 `AnomalyPluginEntryV1`（= `ANOMALY_PLUGIN_V1_ENTRY_NAME`）的函数，宿主调用它来填写描述符：

```c
typedef AnomalyStatusV1 (ANOMALY_CALL *AnomalyPluginEntryV1Fn)(
    AnomalyPluginDescriptorV1* descriptor);
```

入口应校验 `descriptor->struct_size >= sizeof(*descriptor)`，然后原样填入所有字段。

### `AnomalyPluginDescriptorV1`

```c
typedef struct AnomalyPluginDescriptorV1 {
    uint32_t struct_size; uint16_t api_major; uint16_t api_minor;
    AnomalyStringViewV1 id; AnomalyStringViewV1 name;
    AnomalyStringViewV1 author; AnomalyStringViewV1 version;
    AnomalyStatusV1 (ANOMALY_CALL *on_load)(const AnomalyHostApiV1* host, void** plugin_context);
    AnomalyStatusV1 (ANOMALY_CALL *on_start)(void* plugin_context);
    AnomalyStatusV1 (ANOMALY_CALL *on_stop)(void* plugin_context, uint32_t deadline_milliseconds);
    void (ANOMALY_CALL *on_unload)(void* plugin_context);
    void (ANOMALY_CALL *on_update)(void* plugin_context, double delta_seconds);
    void (ANOMALY_CALL *on_draw)(void* plugin_context, const AnomalyUiServiceV1* ui);
} AnomalyPluginDescriptorV1;
```

| 字段 | 说明 |
| --- | --- |
| `api_major` / `api_minor` | 填 `ANOMALY_PLUGIN_API_V1_MAJOR` / `_MINOR` |
| `id` / `name` / `author` / `version` | 插件元数据（与 Manifest 一致） |
| `on_load` | 查询服务、注册资源；通过 `plugin_context` 返回自有上下文指针 |
| `on_start` | 激活 |
| `on_stop` | 在 `deadline_milliseconds` 内停止并提交状态 |
| `on_unload` | 卸载前清理 |
| `on_update` | 每 tick 逻辑更新（Game 域）；可为 `NULL` |
| `on_draw` | 每帧绘制（Render 域）；可为 `NULL` |

回调的线程域见[插件开发 · 生命周期](../developer-guide/plugin-development.md#3-生命周期与入口)。

### `AnomalyHostApiV1`

`on_load` 收到宿主 API：

```c
typedef struct AnomalyHostApiV1 {
    uint32_t struct_size; uint16_t api_major; uint16_t api_minor; void* host_context;
    AnomalyAllocatorV1 allocator;
    AnomalyStatusV1 (ANOMALY_CALL *query_service)(void* host_context,
        AnomalyStringViewV1 service_id, uint32_t minimum_version, const void** service);
} AnomalyHostApiV1;
```

`query_service` 是获取一切服务表的唯一入口。用 C++ wrapper `anomaly::sdk::Host(host).Query<T>(id, version)` 更安全。

---

## `anomaly.core`

- **ID**：`ANOMALY_CORE_SERVICE_V1_ID` = `"anomaly.core"`
- **版本**：`ANOMALY_CORE_SERVICE_V1_VERSION` = 1
- **capability**：无需（但 `read_memory` / `write_memory` 分别需要 `memory-read` / `memory-write` grant）

```c
typedef struct AnomalyCoreServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    void (ANOMALY_CALL *log)(void* user, uint32_t level, AnomalyStringViewV1 message);
    AnomalyStatusV1 (ANOMALY_CALL *read_memory)(void* user, uintptr_t address, AnomalyMutableByteSpanV1 destination);
    AnomalyStatusV1 (ANOMALY_CALL *write_memory)(void* user, uintptr_t address, AnomalyByteSpanV1 source);
    AnomalyStatusV1 (ANOMALY_CALL *plugin_directory)(void* user, char* destination, size_t* inout_size);
} AnomalyCoreServiceV1;
```

| 函数 | 说明 |
| --- | --- |
| `log(level, message)` | 写日志，`level` 见下表 |
| `read_memory(address, destination)` | 读进程内存到 `destination`；需要 `memory-read`，否则 `PERMISSION_DENIED` |
| `write_memory(address, source)` | 写进程内存；需要 `memory-write`，否则 `PERMISSION_DENIED` |
| `plugin_directory(destination, inout_size)` | 取本插件包目录（两段式缓冲协议） |

日志级别 `AnomalyCoreLogLevelV1`：`TRACE=0`、`INFO=1`、`WARNING=2`、`ERROR=3`。

> [!NOTE]
> `anomaly.core` 不会自动授予原始内存访问；新插件应优先使用语义 [NTE snapshot 服务](nte-services.md) 而非自行读游戏对象。

---

## `anomaly.plugin-state`

- **ID**：`"anomaly.plugin-state"` · **版本** 1 · **capability** `configuration`

```c
typedef struct AnomalyPluginStateServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *directory)(void* user, char* destination, size_t* inout_size);
} AnomalyPluginStateServiceV1;
```

`directory` 返回插件私有状态目录（两段式缓冲协议）。持久化的结构化设置建议用 [`anomaly.config`](platform-services.md#anomalyconfig)。

---

## `anomaly.localization`

- **ID**：`"anomaly.localization"` · **版本** 1 · **capability** `ui`

```c
typedef struct AnomalyLocalizationServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *locale)(void* user, char* destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *translate)(void* user,
        AnomalyStringViewV1 key, AnomalyStringViewV1 english_fallback,
        const AnomalyStringViewV1* arguments, size_t argument_count,
        char* destination, size_t* inout_size);
} AnomalyLocalizationServiceV1;
```

| 函数 | 说明 |
| --- | --- |
| `locale(destination, inout_size)` | 当前 locale（如 `zh-CN`），两段式缓冲协议 |
| `translate(key, english_fallback, arguments, argument_count, destination, inout_size)` | 按 `key` 翻译，缺失时用 `english_fallback`；`arguments` 是位置参数数组 |

> [!NOTE]
> `anomaly.localization` 与 `anomaly.ui` 共用 `ui` capability。
