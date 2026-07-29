# 内建插件

运行包默认提供四个面向用户的内建插件。它们和第三方插件一样，都是独立的目录包（`manifest.json` + `plugin.dll`），可以在 **Plugins** 页启停、重载与查看状态。

> [!NOTE]
> 坐标、实体和传送等功能都依赖 [Profile](nte-profiles.md)。活动 Profile 缺少签名、偏移或校验未通过时，插件仍然可以加载，但对应功能会显示为不可用。

## Coordinate Display

| | |
| --- | --- |
| **ID** | `anomaly.builtin.nte-position` |
| **作用** | 只读显示当前玩家坐标、会话（World）状态与快照采样指标。 |
| **依赖服务** | `anomaly.ui`；`anomaly.nte.session`、`anomaly.nte.player`、`anomaly.nte.metrics`（均为 V1，可选） |
| **需要 Profile** | 是（玩家 / 会话数据依赖已验证符号） |

该插件只读取数据，不修改游戏状态。可以用它快速确认坐标和会话数据是否正常。

## Entity ESP

| | |
| --- | --- |
| **ID** | `anomaly.builtin.entity-esp` |
| **作用** | 在前景绘制实体的世界空间包围盒与标签（ESP 覆盖层）。 |
| **依赖服务** | `anomaly.config`、`anomaly.ui`；`anomaly.core`、`anomaly.nte.entities`、`anomaly.ue5.names`（均可选） |
| **需要 Profile** | 是（实体快照与名称解析依赖已验证符号） |

绘制发生在游戏前景，即使管理菜单折叠也会显示。设置项通过 `anomaly.config` 持久化。

## Pink Paw Heist ESP

| | |
| --- | --- |
| **ID** | `anomaly.builtin.pink-paw-heist-esp` |
| **作用** | 显示粉爪大劫案中的战利品和撤离点，战利品可按最低价值筛选。 |
| **依赖服务** | `anomaly.ui`、`anomaly.config`、`anomaly.interop.signature`、`anomaly.ue5.framework`、`anomaly.ue5.names`；`anomaly.nte.session/player/player-teleport/entities/actors`、`anomaly.font`、`anomaly.texture`（均为 V1，可选） |
| **需要 Profile** | 通用会话、玩家、实体、Actor 与传送服务需要；Pink Paw 专用拾取签名和布局由插件自带 |

插件会在游戏中绘制标签，并在插件窗口中列出物品名称、价值、坐标和撤离点状态。RobBank
可拾取判定和 native pickup 调用只存在于插件内；宿主只提供签名扫描、Game 回调、名称解析、
原始内存读取以及上述通用 NTE 服务。

## Custom UID

| | |
| --- | --- |
| **ID** | `anomaly.local.nte.fake-uid` |
| **作用** | 通过签名解析与对象 / 名称服务，修改界面上显示的 UID。 |
| **依赖服务** | `anomaly.config`、`anomaly.interop.signature`、`anomaly.scheduler`、`anomaly.ue5.names`、`anomaly.ue5.objects`；`anomaly.ui`、`anomaly.window`（可选） |
| **需要 Profile** | 通用对象与名称服务需要；FakeUID 专用签名和布局由插件自带 |

> [!NOTE]
> Custom UID 的 Manifest 当前声明 `builds: ["nte-*"]`，但插件内置的签名和布局只在已知游戏版本上验证过。宿主仅提供通用签名扫描、调度、对象快照和名称解析；插件会在运行时检查自己的签名、对象布局和 vtable，检查不通过时显示为不可用，不会强行写入。

## 管理插件

在 **Plugins** 页你可以：

- 搜索 / 过滤 / 排序已安装插件。
- 启用、停用或重载单个插件。
- 查看插件状态、失败原因和回调耗时。
- 通过 **Open / Hide** 控制插件窗口；**Reload all** 显式触发全量重载。

下载安装第三方插件见[第三方插件](third-party-plugins.md)；自己写插件见[插件开发](../developer-guide/plugin-development.md)。
