# 插件开发

本页带你用 Anomaly SDK 从零写一个插件。**完整的接口签名在 [API 参考](../api-reference/README.md)**；本页讲流程与要点。仓库 [`examples/`](../../examples/README.md) 内有四个可直接构建的示例。

要新建独立插件仓库，可从 [Anomaly 插件模板](https://github.com/AnomalyNTE/Anomaly-Plugin-Template) 开始：点击 GitHub 的 **Use this template**，再按模板说明替换插件 ID、名称、作者和 capability。模板已包含 CMake 工程、Manifest、本地化资源与可运行的 UI Gallery 示例。

## 核心概念

一个插件是一个**目录包**：

```text
Anomaly\plugins\MyPlugin\
  manifest.json     声明 ID、版本、API 范围、依赖、所需服务与 capability
  plugin.dll        导出 AnomalyPluginEntryV1
  assets\           （可选）资源
  dependency.dll    （可选）native 依赖
```

- 加载时整个包被复制到**不可变影子代际**，资源与 native 依赖与 DLL 使用同一快照。
- 根级 DLL 与没有 Manifest 的目录**不会被加载**。
- 插件通过导出 `AnomalyPluginEntryV1` 并填写 `AnomalyPluginDescriptorV1` 接入。ABI v1 入口只承担**生命周期**与 **`query_service`**。

## 1. 安装 SDK

插件仓库只需要安装后的 SDK，不需要 Runtime 源码或 import library：

```powershell
cmake --install C:\path\to\anomaly-build --prefix C:\AnomalySDK --component SDK
```

## 2. 最小插件工程

```cmake
cmake_minimum_required(VERSION 3.22)
project(MyPlugin LANGUAGES CXX)
find_package(AnomalySDK CONFIG REQUIRED)
anomaly_add_plugin(my_plugin
  SOURCES plugin.cpp
  MANIFEST manifest.json
  PACKAGE_NAME MyPlugin)
```

`anomaly_add_plugin` 固定输出 `plugin.dll` 并把 Manifest 放入同一包目录。

源码入口：`<anomaly/sdk/anomaly_sdk.h>`（C）或 `<anomaly/sdk/cpp.hpp>`（C++，提供 `Host`、`ServiceRef`、`UiWindow` 等轻量 wrapper）。

## 3. 生命周期与入口

一个最小的、只显示文本的插件（改编自 [`examples/tick_counter`](../../examples/tick_counter/plugin.cpp)）：

```cpp
#include "anomaly/sdk/cpp.hpp"

namespace {
const AnomalyUiServiceV1* g_ui{};

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    const auto ui = anomaly::sdk::Host(host).Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    if (!ui) return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    g_ui = ui.get();
    *context = &g_ui;
    return anomaly::sdk::Ok();
}
AnomalyStatusV1 ANOMALY_CALL Start(void*) { return anomaly::sdk::Ok(); }
AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) { return anomaly::sdk::Ok(); }
void ANOMALY_CALL Unload(void*) { g_ui = nullptr; }
void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr) ui = g_ui;
    anomaly::sdk::UiWindow window(ui, "My Plugin");
    if (!window) return;
    ui->text(ui->user, anomaly::sdk::StringView("Hello from a plugin"));
}
}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor))
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.example.my-plugin"),
        anomaly::sdk::StringView("My Plugin"),
        anomaly::sdk::StringView("Me"), anomaly::sdk::StringView("1.0.0"),
        Load, Start, Stop, Unload, /*on_update*/ nullptr, Draw};
    return anomaly::sdk::Ok();
}
```

生命周期回调及其线程亲和性：

| 回调 | 何时 | 线程域 |
| --- | --- | --- |
| `on_load` | 加载后，查询服务、注册资源 | Lifecycle |
| `on_start` | 激活 | Lifecycle |
| `on_update(delta)` | 每 tick 逻辑更新 | Game |
| `on_draw(ui)` | 每帧绘制 | Render |
| `on_stop(deadline)` | 停止，提交状态 | Lifecycle |
| `on_unload` | 卸载前清理 | Lifecycle |

完整入口 / 描述符定义见 [生命周期与 Core 服务](../api-reference/lifecycle-and-core.md)。

## 4. 查询服务并优雅降级

服务通过 `query_service` 获取，当前统一使用版本 1。C++ wrapper `Host::Query` 会自动校验 `struct_size`、`api_major` 与 `service_version`，失败返回空 `ServiceRef`：

```cpp
const auto config = anomaly::sdk::Host(host).Query<AnomalyConfigServiceV1>(
    ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION);
if (!config) { /* 该服务不可用，降级处理 */ }
```

> [!IMPORTANT]
> 插件必须能处理服务不可用的情况。UI 服务可能还没就绪；NTE 服务也会因为没有活动 Profile，或相关 Feature 校验失败而返回 `UNAVAILABLE`。调用前要检查 `struct_size`，调用后要检查 status。

未授权的服务返回 `ANOMALY_STATUS_V1_PERMISSION_DENIED`，与尚未发布时的 `ANOMALY_STATUS_V1_UNAVAILABLE` 区分。

## 5. Manifest v2

每个插件必须有 `manifest.json`。示例：

```json
{
  "schemaVersion": 2,
  "id": "anomaly.example.my-plugin",
  "name": "My Plugin",
  "description": "Shows current player and session diagnostics.",
  "author": "Me",
  "license": "MIT",
  "version": "1.0.0",
  "entry": "plugin.dll",
  "api": {"major": 1, "minMinor": 0, "maxMinor": 0},
  "games": ["nte"],
  "builds": ["nte-*"],
  "loadPhase": "game-ready",
  "services": [
    {"id": "anomaly.ui", "minVersion": 1},
    {"id": "anomaly.config", "minVersion": 1}
  ],
  "capabilities": ["ui", "configuration"]
}
```

字段完整定义见 [Manifest 与 capability](../api-reference/manifest-and-capabilities.md) 与 [`schemas/plugin-manifest.schema.json`](../../schemas/plugin-manifest.schema.json)。

- `id` 是规范化 ID（点分小写）。
- `description` 是管理界面“关于”区域使用的插件简介；插件可通过本地化目录中的 `plugin.description` 提供翻译。
- `builds` 是 build 兼容性声明，可以写 `nte-*` 或精确 fingerprint。当前生产 Runtime 不计算 fingerprint，加载时也不用该字段拦截插件，因此不能把它当作运行时安全检查。
- `license` 是单个 SPDX identifier（公开分发时必填）。

## 6. Capability 与授权

**每个非可选 required service 必须声明对应 capability**，否则包在 DLL 映射前被拒绝。声明未知 capability，或某个 required service 没有映射 / 没有对应 capability，都会导致包不可加载。`anomaly.core` 本身不需要 capability。

capability ↔ service 对应关系见 [Manifest 与 capability · capability 映射](../api-reference/manifest-and-capabilities.md#capability-映射)。

原始内存访问是**独立 grant**：

- `read_memory` 需要 `memory-read`
- `write_memory` 需要 `memory-write`

缺失时对应调用返回 `PERMISSION_DENIED`。

> [!NOTE]
> capability 机制约束服务可见性与资源归属，**不是**同进程 native 代码的安全沙箱。新插件应优先使用语义 NTE snapshot 服务，而不是自行读取游戏对象内存。

### 周围拾取

需要拾取周围 `PropBox_`、`InteractBox_` 或随机物品 Actor 时，声明 `nte-pickup` capability
并查询 `anomaly.nte.pickup`。在 Game Update 中提交 `AnomalyNtePickupRequestV1`，在 Draw/UI
中读取 `AnomalyNtePickupSnapshotV1`。请求只表示已排队，确认计数和 2 秒截止时间由 Host
维护；`QUEUED` 或 `CHECKING` 时不应重复提交。

插件不得自行扫描 GObjects/World、解析 `BPGetInteractEntries` 或 `BPCanTryInteract`，也不得
直接调用 UE `ProcessEvent`、修改 UFunction flags 或读取 `bInteractFinish`。这些行为属于
NTE Adapter 的 Profile gate 和线程域，避免实体 generation、对象生命周期和确认逻辑在插件间分叉。
确认优先复用实体缓存和直接状态字节，仅在截止时调用一次 `BPCanTryInteract`。

### 地图地标传送

需要显示地图上所有可传送地标时，声明 `nte-map-landmarks` capability，并查询
`anomaly.nte.map-landmarks`。地标目录由 Host 从活动 Profile 验证的 `TeleportPoint`
DataTable 构造；插件只消费 `TeleportID`、所属世界、世界坐标、有效目的坐标、楼层和类型，
不自行扫描 GObjects、解析 DataTable 或调用 `ProcessEvent`。

目录用 `sequence` 标识不可变版本。插件缓存一次完整枚举；仅在 `sequence()` 变化时重新读取，
并确认每个 `snapshot_at()` 返回的 `snapshot.sequence` 仍等于目标序列。选择状态保存
`sequence + index`，不要保存 UE 指针或把 UI 文本重新解释为 ID：

```cpp
const auto sequence = service->sequence(service->user);
if (sequence != 0 && sequence != cached_sequence) {
    std::vector<AnomalyNteMapLandmarkSnapshotV1> next;
    const auto count = service->count(service->user);
    next.reserve(count);
    bool complete = true;
    for (std::uint32_t index = 0; index < count; ++index) {
        AnomalyNteMapLandmarkSnapshotV1 item{sizeof(item)};
        const auto status = service->snapshot_at(service->user, index, &item);
        if (status.code != ANOMALY_STATUS_V1_OK || item.sequence != sequence) {
            complete = false;
            break;
        }
        next.push_back(item);
    }
    if (complete && service->sequence(service->user) == sequence) {
        landmarks = std::move(next);
        cached_sequence = sequence;
    }
}
```

`on_draw` 属于 Render 域，只负责显示选项并把所选 `sequence`、`index` 和 mode 放入插件自己的
待处理状态；`on_update` 属于 Game 域，在开发者模式确认后构造
`AnomalyNteMapLandmarkTeleportRequestV1` 并调用 `teleport`。若目录已刷新，旧请求返回
`NOT_FOUND`，插件刷新列表并要求重新选择，不用猜测新的索引。

仓库内的 `plugins/TeleportLandmarksProbe` 是该模式的开发者调试实现：它只依赖 `anomaly.ui`
和 `anomaly.nte.map-landmarks`，不包含签名、偏移、原始内存读取或 UE 调用逻辑。

## 7. 资源与所有权

所有由 Platform / Interop / IPC 创建的 handle 都**绑定 plugin generation**：

- Scheduler / IPC 的未开始工作在 stop / reload 时取消。
- tracked Patch 在 release / stop / reload 时恢复原字节；可恢复的 Patch 必须使用 `anomaly.interop.patch`。
- function Hook 的 detour 必须以 `begin_callback` / `end_callback` 包围；当前 Hook 只支持 function kind。
- 插件 callback 首次故障后停止后续调用；失败的 stop 代际保持隔离。

宿主分配的内存由宿主 allocator 释放；公开边界不传递 C++ STL、异常、`ImGuiContext*`、裸 `UObject*` 或宿主内部对象。

## 8. 本地开发循环

```powershell
cmake -S . -B build -DAnomalySDK_DIR=C:\AnomalySDK\lib\cmake\AnomalySDK
cmake --build build --config Debug
anomaly-plugin validate build\package\MyPlugin
anomaly-plugin pack build\package\MyPlugin --output .\dist
anomaly-test-host --plugin .\dist\anomaly.example.my-plugin --reload 10 --ticks 60
```

`anomaly-test-host` 是**无游戏**的离线 fixture：它加载你的包、跑若干 tick 和 reload，用来验证生命周期与降级行为。该环境没有真实游戏适配器，NTE 服务会保持 `UNAVAILABLE`，但插件包本身仍应能够正常加载和卸载。

## 9. 部署与热重载

把通过 TestHost 的包复制到 `Anomaly\plugins\<包目录>`。文件监控（debounce）会：

1. 先校验新代际；
2. 停止旧 Scope；
3. 只重载**变更包及其依赖闭包中的下游包**。

管理界面的 **Reload all** 可显式触发全量重载。激活前会预检 static / 受支持的 delay-load import、private CRT / system DLL 与同名已加载模块；动态 `LoadLibrary` 的模块不在预检范围，插件必须自行避免进程级 DLL 名称冲突。

## 10. 分发

- Manifest 的 `license` 填单个 SPDX identifier；资产或 native dependency 的独立许可证随包提供。
- Debug / Release 都必须通过同一 ABI 契约。
- 优先使用语义 snapshot 服务；确需原始内存时显式声明 `memory-read` / `memory-write`。

要让用户直接在 **Plugins > 可用** 中安装，需要把目录包压成根目录结构正确的 ZIP，再发布 `pluginmaster.json`。字段、发布顺序和本地 `file://` 测试见[发布第三方插件](plugin-distribution.md)。

## 示例一览

| 示例 | 演示 |
| --- | --- |
| [`hello_ui`](../../examples/hello_ui/plugin.c) | 纯 C 的 scoped Window / Font / Texture / Input / Hotkey 与 host-owned UI；资源未就绪时降级绘制 |
| [`tick_counter`](../../examples/tick_counter/plugin.cpp) | C++ RAII + Game Thread Tick |
| [`reliable_config`](../../examples/reliable_config/plugin.cpp) | 通过 Config ABI 注册 JSON Schema、读取设置，`on_stop` 原子提交 |
| [`nte_inspector`](../../examples/nte_inspector/plugin.cpp) | Session lifecycle event、bounded EntityPage、class-name 解析与 Host snapshot metrics |

## 相关

- [API 参考](../api-reference/README.md) — 完整接口
- [发布第三方插件](plugin-distribution.md) — ZIP、插件列表与发布流程
- [架构概览 · ABI 边界](architecture.md#abi-边界)
- [贡献指南](contributing.md)
