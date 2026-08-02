# Manifest 与 capability

对应 schema：[`schemas/plugin-manifest.schema.json`](../../schemas/plugin-manifest.schema.json)；capability 策略源：`src/plugin/plugin_capability_policy.cpp`。流程见[插件开发](../developer-guide/plugin-development.md)。

## Manifest v2

每个插件目录包必须含 `manifest.json`（`schemaVersion` 固定为 `2`）。

### 字段

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `schemaVersion` | ✅ | 常量 `2` |
| `id` | ✅ | 规范化 ID：点分小写标签（如 `anomaly.example.my-plugin`），3–255 字符 |
| `name` | ✅ | 显示名，1–128 字符 |
| `description` | 否 | 插件简介，1–512 字符；宿主在插件详情的“关于”区域显示 |
| `version` | ✅ | SemVer 字符串 |
| `entry` | ✅ | 入口 DLL 文件名，必须以 `.dll` 结尾 |
| `api` | ✅ | `{ "major", "minMinor", "maxMinor" }`，声明支持的插件 API 版本范围 |
| `games` | ✅ | 目标 game id 数组（1–16 项，如 `["nte"]`） |
| `builds` | ✅ | build 兼容性声明（如 `["nte-*"]` 或精确 fingerprint）；每个模式属于一个已声明 game |
| `loadPhase` | ✅ | 目前只允许 `"game-ready"` |
| `author` | — | 作者名 |
| `license` | — | 单个 SPDX license identifier（**公开分发时必填**） |
| `audience` | — | `"user"` 或 `"developer"` |
| `dependencies` | — | 插件依赖数组，见下 |
| `services` | — | 所需服务数组，见下 |
| `capabilities` | — | 声明的 capability 数组（唯一，≤64 项） |

### 依赖与服务

```jsonc
// dependencies[] 元素
{ "id": "<qualifiedId>", "version": "<range>", "optional": false }
// services[] 元素
{ "id": "<qualifiedId>", "minVersion": 1, "optional": false }
```

### 示例

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
    {"id": "anomaly.nte.entities", "minVersion": 1, "optional": true}
  ],
  "capabilities": ["ui", "nte-entity-snapshot"]
}
```

## 加载规则

- 插件必须是**含 Manifest 的目录包**；根级 DLL 与无 Manifest 的目录不会被加载。
- 加载时整个包被复制到不可变影子代际；入口与资源路径在打开文件句柄后再次验证仍位于包根目录内。
- 激活前预检 static / 受支持 delay-load import、private CRT / system DLL 与同名已加载模块；无法隔离的依赖冲突会被拒绝。

`DiscoverPluginCatalog` 只在调用方传入 `PluginCompatibilityContext` 时才校验 `games`、`builds`、API 范围和服务版本。当前生产 Runtime 发现本地插件时没有传入该上下文，因此 `builds` 会被解析，但不会用来阻止加载。插件必须依靠服务可用性、签名和布局校验做实际降级。

## Capability 强制校验

capability 约束**服务可见性与资源归属**（不是 native 代码沙箱）。校验发生在 DLL 映射前。以下任一情况导致该插件的授权**不可强制执行 / 被拒绝**：

- 声明了**未知 capability**。
- 某个**非可选**、且非 `anomaly.core` 的 required service 没有对应 capability 映射。
- 某个非可选 required service 有映射，但插件**没有声明**对应 capability。
- `schemaVersion` 不是当前值（`2`）。

运行时若查询未授权的服务，返回 `ANOMALY_STATUS_V1_PERMISSION_DENIED`（与尚未发布时的 `UNAVAILABLE` 区分）。

> [!NOTE]
> 声明为 `optional: true` 的 service 不参与上述强制映射检查，其不可用也不会阻止插件加载——用于优雅降级。

## Capability 映射

每个（非可选）required service 需要下表对应的 capability：

| Service ID | 所需 capability |
| --- | --- |
| `anomaly.core` | （无需；`read_memory` / `write_memory` 另见下方内存 grant） |
| `anomaly.plugin-state` | `configuration` |
| `anomaly.config` | `configuration` |
| `anomaly.storage` | `storage` |
| `anomaly.runtime-info` | `runtime-info` |
| `anomaly.diagnostics` | `diagnostics` |
| `anomaly.scheduler` | `scheduler` |
| `anomaly.ipc` | `ipc` |
| `anomaly.commands` | `commands` |
| `anomaly.notifications` | `notifications` |
| `anomaly.interop.signature` | `interop-signature` |
| `anomaly.interop.hook` | `interop-hook` |
| `anomaly.interop.patch` | `interop-patch` |
| `anomaly.ui` | `ui` |
| `anomaly.localization` | `ui` |
| `anomaly.window` | `ui-window` |
| `anomaly.font` | `ui-font` |
| `anomaly.texture` | `ui-texture` |
| `anomaly.input` | `input` |
| `anomaly.ue5.build` | `ue5-build` |
| `anomaly.ue5.ahud` | `ue5-ahud` |
| `anomaly.ue5.framework` | `game-events` |
| `anomaly.ue5.names` | `ue5-names` |
| `anomaly.ue5.objects` | `ue5-objects` |
| `anomaly.ue5.world` | `ue5-world` |
| `anomaly.nte.build` | `nte-build` |
| `anomaly.nte.esc-menu-button` | `nte-esc-menu-button` |
| `anomaly.nte.session` | `nte-session-snapshot` |
| `anomaly.nte.metrics` | `nte-snapshot-metrics` |
| `anomaly.nte.player` | `nte-player-snapshot` |
| `anomaly.nte.player-teleport` | `nte-player-teleport` |
| `anomaly.nte.entities` | `nte-entity-snapshot` |
| `anomaly.nte.actors` | `nte-actor-snapshot` |

### 独立 grant（不绑定服务查询）

| capability | 授予 |
| --- | --- |
| `memory-read` | `anomaly.core` 的 `read_memory` |
| `memory-write` | `anomaly.core` 的 `write_memory` |
| `entity-esp` | ABI v1 兼容的 `anomaly.ui` ESP 绘制调用（`draw_entity_bbox` / `draw_entity_box3d` / `draw_entity_label`）；新插件应声明 `ue5-ahud` 并订阅 `anomaly.ue5.ahud` |

以上三个 capability 是已知 capability，但不通过 service 映射自动派生——插件按需在 `capabilities` 中显式声明。

## 状态码

插件调用返回 `AnomalyStatusV1`，`code` 取值见 [API 参考总览 · 状态码](README.md#状态码-anomalystatuscodev1)。与授权相关的两个常见码：

- `ANOMALY_STATUS_V1_PERMISSION_DENIED` — 未声明所需 capability / grant。
- `ANOMALY_STATUS_V1_UNAVAILABLE` — 服务 / Feature 尚未发布或已降级。
