# API 参考

这是 Anomaly 插件 **纯 C ABI v1** 的完整参考。权威定义始终以 `include/anomaly/sdk/` 下的头文件为准；本参考与之对应并补充语义说明。

面向流程的讲解见[插件开发](../developer-guide/plugin-development.md)。

## 页面

| 页面 | 内容 |
| --- | --- |
| [生命周期与 Core 服务](lifecycle-and-core.md) | 插件入口、Host API、描述符、`anomaly.core`、`plugin-state`、`localization` |
| [平台作用域服务](platform-services.md) | `config`、`storage`、`runtime-info`、`diagnostics`、`scheduler`、`commands`、`notifications` |
| [Interop 与内存](interop-and-memory.md) | `interop.signature`、`interop.hook`、`interop.patch`、内存 grant |
| [插件间 IPC](ipc.md) | `anomaly.ipc` |
| [WebSocket 广播](websocket.md) | `anomaly.websocket` |
| [UI 服务](ui-services.md) | `anomaly.ui`、`window`、`font`、`texture`、`input` |
| [UE5 服务](ue5-services.md) | `ue5.build`、`ahud`、`framework`、`names`、`objects`、`world` |
| [NTE 服务](nte-services.md) | `nte.build`、`session`、`player`、`player-teleport`、`navigation`、`entities`、`actors`、`metrics` |
| [Manifest 与 capability](manifest-and-capabilities.md) | Manifest v2 schema、capability 映射、状态码 |

## ABI 约定

- 头文件为纯 C（`extern "C"`），可同时被 C 与 C++ 编译。C++ 可额外包含 `<anomaly/sdk/cpp.hpp>`。
- 调用约定宏 `ANOMALY_CALL` 在 Win32 上是 `__cdecl`。
- 插件导出宏 `ANOMALY_SDK_EXPORT`（`__declspec(dllexport)`）。
- 不跨越 DLL 边界传递 STL、异常、RTTI 对象、裸 UE 对象或 `ImGuiContext*`。

### 版本常量（`version.h`）

| 常量 | 值 |
| --- | --- |
| `ANOMALY_SDK_VERSION_STRING` | 配置的发行版本（正式 tag 构建与 tag 一致，例如 `"1.0.1"`） |
| `ANOMALY_PLUGIN_API_V1_MAJOR` | `1` |
| `ANOMALY_PLUGIN_API_V1_MINOR` | `0` |
| `ANOMALY_PLUGIN_V1_ENTRY_NAME` | `"AnomalyPluginEntryV1"` |

## 基础类型（`base.h`）

```c
typedef struct AnomalyStringViewV1 { const char* data; size_t size; } AnomalyStringViewV1;
typedef struct AnomalyByteSpanV1 { const uint8_t* data; size_t size; } AnomalyByteSpanV1;
typedef struct AnomalyMutableByteSpanV1 { uint8_t* data; size_t size; } AnomalyMutableByteSpanV1;
typedef struct AnomalyGenerationHandleV1 { uint64_t id; uint64_t generation; } AnomalyGenerationHandleV1;
```

### 状态码 `AnomalyStatusCodeV1`

```c
typedef struct AnomalyStatusV1 { uint32_t code; uint32_t reserved; AnomalyStringViewV1 message; } AnomalyStatusV1;
```

| 值 | 名称 | 含义 |
| --- | --- | --- |
| 0 | `ANOMALY_STATUS_V1_OK` | 成功 |
| 1 | `ANOMALY_STATUS_V1_INVALID_ARGUMENT` | 参数非法 |
| 2 | `ANOMALY_STATUS_V1_UNAVAILABLE` | 服务 / Feature 尚未发布或已降级 |
| 3 | `ANOMALY_STATUS_V1_NOT_FOUND` | 目标不存在（如无保存文档、stale cursor） |
| 4 | `ANOMALY_STATUS_V1_BUFFER_TOO_SMALL` | 缓冲区不足（配合两段式缓冲协议） |
| 5 | `ANOMALY_STATUS_V1_FAILED` | 一般失败 |
| 6 | `ANOMALY_STATUS_V1_TIMEOUT` | 超时 |
| 7 | `ANOMALY_STATUS_V1_PERMISSION_DENIED` | 未授权（capability / grant 缺失） |
| 8 | `ANOMALY_STATUS_V1_CONFLICT` | 冲突 |
| 9 | `ANOMALY_STATUS_V1_CANCELLED` | 已取消 |

C++ 辅助：`anomaly::sdk::Ok()`、`Succeeded(status)`。

### Feature 状态 `AnomalyFeatureStateV1`

`ANOMALY_FEATURE_V1_UNAVAILABLE = 0`，`ANOMALY_FEATURE_V1_AVAILABLE = 1`。用于 `feature_state(feature_id)` 查询。

### 分配器 `AnomalyAllocatorV1`

宿主通过 `AnomalyHostApiV1::allocator` 暴露 `allocate` / `reallocate` / `release`。**宿主分配的内存由宿主 allocator 释放。**

## 服务查询模型

宿主 `AnomalyHostApiV1::query_service(host_context, service_id, minimum_version, &table)` 返回一个 opaque 服务表指针。每张服务表都以相同的三字段前缀开头：

```c
uint32_t struct_size;      // 本表实际大小
uint32_t service_version;  // 本表版本
void*    user;             // 调用每个函数时作为第一个参数回传
```

**消费者在使用一张表前必须**校验 `struct_size` 足够覆盖将要调用的字段，并确认 `service_version == 1`。C++ wrapper `anomaly::sdk::Host::Query<Service>(id, 1)` 封装了这些检查（含 `api_major` 校验），失败返回空 `ServiceRef<Service>`。当前 ABI 不提供旧版服务表或版本别名。

> [!IMPORTANT]
> 服务表属于**一个 Host 生命周期 generation**。来自已停止 / 已替换 generation 的缓存表只能返回 `UNAVAILABLE`（标量查询返回 0），不会在后续 Start generation 上恢复。每次生命周期后重新查询。

## 两段式缓冲协议

许多返回可变长度数据的函数使用形如 `(..., char* destination, size_t* inout_size)` 或 `MutableByteSpan + size_t*` 的签名：

1. 先以 `destination = NULL`、`*inout_size = 0` 调用 → 返回所需字节数（写入 `*inout_size`），status 通常为 `OK` 或 `BUFFER_TOO_SMALL`。
2. 分配缓冲后再次调用 → 写入数据，`*inout_size` 更新为实际写入长度。

## Generation Handle

`AnomalyGenerationHandleV1 { id, generation }` 标识一个宿主资源或对象快照。所有由 Platform / Interop / IPC 创建的 handle 都绑定 plugin generation；stale handle 会被拒绝，而不是解析到后来的对象身份。资源生命周期见[架构概览 · 所有权与生命周期](../developer-guide/architecture.md#所有权与生命周期)。

## 服务索引

| 服务 ID | 当前版本 | 所需 capability | 参考 |
| --- | --- | --- | --- |
| `anomaly.core` | 1 | （内存操作另需 `memory-read` / `memory-write`） | [Core](lifecycle-and-core.md#anomalycore) |
| `anomaly.plugin-state` | 1 | `configuration` | [plugin-state](lifecycle-and-core.md#anomalyplugin-state) |
| `anomaly.localization` | 1 | `ui` | [localization](lifecycle-and-core.md#anomalylocalization) |
| `anomaly.config` | 1 | `configuration` | [config](platform-services.md#anomalyconfig) |
| `anomaly.storage` | 1 | `storage` | [storage](platform-services.md#anomalystorage) |
| `anomaly.runtime-info` | 1 | `runtime-info` | [runtime-info](platform-services.md#anomalyruntime-info) |
| `anomaly.diagnostics` | 1 | `diagnostics` | [diagnostics](platform-services.md#anomalydiagnostics) |
| `anomaly.scheduler` | 1 | `scheduler` | [scheduler](platform-services.md#anomalyscheduler) |
| `anomaly.commands` | 1 | `commands` | [commands](platform-services.md#anomalycommands) |
| `anomaly.notifications` | 1 | `notifications` | [notifications](platform-services.md#anomalynotifications) |
| `anomaly.interop.signature` | 1 | `interop-signature` | [signature](interop-and-memory.md#anomalyinteropsignature) |
| `anomaly.interop.hook` | 1 | `interop-hook` | [hook](interop-and-memory.md#anomalyinterophook) |
| `anomaly.interop.patch` | 1 | `interop-patch` | [patch](interop-and-memory.md#anomalyinteroppatch) |
| `anomaly.ipc` | 1 | `ipc` | [ipc](ipc.md) |
| `anomaly.websocket` | 1 | `websocket` | [websocket](websocket.md) |
| `anomaly.ui` | 1 | `ui` | [ui](ui-services.md#anomalyui) |
| `anomaly.window` | 1 | `ui-window` | [window](ui-services.md#anomalywindow) |
| `anomaly.font` | 1 | `ui-font` | [font](ui-services.md#anomalyfont) |
| `anomaly.texture` | 1 | `ui-texture` | [texture](ui-services.md#anomalytexture) |
| `anomaly.input` | 1 | `input` | [input](ui-services.md#anomalyinput) |
| `anomaly.ue5.build` | 1 | `ue5-build` | [ue5.build](ue5-services.md#anomalyue5build) |
| `anomaly.ue5.ahud` | 1 | `ue5-ahud` | [ue5.ahud](ue5-services.md#anomalyue5ahud) |
| `anomaly.ue5.framework` | 1 | `game-events` | [ue5.framework](ue5-services.md#anomalyue5framework) |
| `anomaly.ue5.names` | 1 | `ue5-names` | [ue5.names](ue5-services.md#anomalyue5names) |
| `anomaly.ue5.objects` | 1 | `ue5-objects` | [ue5.objects](ue5-services.md#anomalyue5objects) |
| `anomaly.ue5.world` | 1 | `ue5-world` | [ue5.world](ue5-services.md#anomalyue5world) |
| `anomaly.nte.build` | 1 | `nte-build` | [nte.build](nte-services.md#anomalyntebuild) |
| `anomaly.nte.esc-menu-button` | 1 | `nte-esc-menu-button` | [nte.esc-menu-button](nte-services.md#anomalynteesc-menu-button) |
| `anomaly.nte.session` | 1 | `nte-session-snapshot` | [nte.session](nte-services.md#anomalyntesession) |
| `anomaly.nte.player` | 1 | `nte-player-snapshot` | [nte.player](nte-services.md#anomalynteplayer) |
| `anomaly.nte.player-teleport` | 1 | `nte-player-teleport` | [nte.player-teleport](nte-services.md#anomalynteplayer-teleport) |
| `anomaly.nte.navigation` | 1 | `nte-navigation` | [nte.navigation](nte-services.md#anomalyntenavigation) |
| `anomaly.nte.entities` | 1 | `nte-entity-snapshot` | [nte.entities](nte-services.md#anomalynteentities) |
| `anomaly.nte.actors` | 1 | `nte-actor-snapshot` | [nte.actors](nte-services.md#anomalynteactors) |
| `anomaly.nte.metrics` | 1 | `nte-snapshot-metrics` | [nte.metrics](nte-services.md#anomalyntemetrics) |

所有公开服务均使用版本 1。可选服务仍可能因 capability、活动 Profile 或 Feature gate 而不可用，插件必须按服务粒度降级。
