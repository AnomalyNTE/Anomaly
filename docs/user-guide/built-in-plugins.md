# 内建插件

运行包默认提供六个面向用户的内建插件。它们和第三方插件一样，都是独立的目录包（`manifest.json` + `plugin.dll`），可以在 **Plugins** 页启停、重载与查看状态。

> [!NOTE]
> 坐标、实体 ESP 和 WalletCollector 等功能都依赖 [Profile](nte-profiles.md)。活动 Profile 缺少签名、偏移或校验未通过时，插件仍然可以加载，但对应功能会显示为不可用。

## Coordinate Display

| | |
| --- | --- |
| **ID** | `anomaly.builtin.nte-position` |
| **作用** | 只读显示当前玩家坐标、会话（World）状态与快照采样指标。 |
| **依赖服务** | `anomaly.ui`；`anomaly.nte.session`、`anomaly.nte.player`、`anomaly.nte.metrics`（均为 V1，可选） |
| **需要 Profile** | 是（玩家 / 会话数据依赖已验证符号） |

该插件只读取数据，不修改游戏状态。可以用它快速确认坐标和会话数据是否正常。

## Nearby Pickup

| | |
| --- | --- |
| **ID** | `anomaly.local.nte-pickup-demo` |
| **作用** | 点击一次拾取半径内的 `PropBox_`、`InteractBox_` 与随机物品 Actor，并显示 Host 确认状态。 |
| **依赖服务** | `anomaly.ui`、`anomaly.localization`、`anomaly.nte.pickup`（V1） |
| **需要 Profile** | 是；拾取反射 ABI、对象/名称/玩家/实体布局必须通过 `nte-pickup-layout-v1`。 |

界面中的 `nearby`、`triggered`、`confirmed`、`checking`、`unconfirmed` 和 `skipped` 均来自
框架 ABI。`triggered` 只表示交互调用完成；确认窗口最多 2 秒，超时会保留 `OK` 并单独显示
`unconfirmed`，不会把请求整体标为失败。确认期间按钮保持禁用；框架通过后续实体缓存或
`bInteractFinish` 变化快速确认，只在截止时做一次最终可交互检查。

## Entity ESP

| | |
| --- | --- |
| **ID** | `anomaly.builtin.entity-esp` |
| **作用** | 使用 UE 原生 AHUD 绘制实体的世界空间包围盒与标签。 |
| **依赖服务** | `anomaly.config`、`anomaly.ui`、`anomaly.ue5.ahud`；`anomaly.core`、`anomaly.nte.entities`、`anomaly.ue5.names`（均可选） |
| **需要 Profile** | 是（实体快照、名称解析和 AHUD 绘制依赖已验证符号） |

插件窗口和设置菜单继续使用 ImGui；实体边界框和标签由 AHUD 在 Game 线程绘制，即使管理菜单折叠也会显示。设置项通过 `anomaly.config` 持久化。

## Pink Paw Heist ESP

| | |
| --- | --- |
| **ID** | `anomaly.builtin.pink-paw-heist-esp` |
| **作用** | 显示粉爪大劫案中的战利品和撤离点，战利品可按最低价值筛选。 |
| **依赖服务** | `anomaly.ui`、`anomaly.config`、`anomaly.interop.signature`、`anomaly.ue5.framework`、`anomaly.ue5.ahud`、`anomaly.ue5.names`；`anomaly.websocket`、`anomaly.nte.session/player/player-teleport/entities/actors`、`anomaly.font`、`anomaly.texture`（均为 V1，可选） |
| **需要 Profile** | 是；物品与撤离点绘制需要已验证的 `ue5.ahud`，通用会话、玩家、实体与 Actor 服务也由 Profile 控制；Pink Paw 专用拾取签名和布局由插件自带 |

插件窗口和设置菜单继续使用 ImGui；物品边框、标签与撤离点改由 UE 原生 AHUD 绘制，即使管理菜单
折叠也会显示。窗口中仍会列出物品名称、价值、坐标和撤离点状态。RobBank 可拾取判定和 native
pickup 调用只存在于插件内；宿主提供签名扫描、Game 回调、AHUD 绘制、名称解析、原始内存读取
以及上述通用 NTE 服务。

在粉爪地图中，插件还会通过 Runtime 的本地 `anomaly.websocket` 服务广播兼容的坐标消息、完整战利品
快照以及战利品增量；离开粉爪地图时会发送清空事件。插件设置中的 WebSocket 实时定位默认开启，
默认监听地址为 `ws://127.0.0.1:14514`，端口可在窗口中修改。战利品筛选由地图前端完成，不受 ESP
菜单的价值或可拾取筛选影响。

## Custom UID

| | |
| --- | --- |
| **ID** | `anomaly.local.nte.fake-uid` |
| **作用** | 通过签名解析与对象 / 名称服务，修改界面上显示的 UID。 |
| **依赖服务** | `anomaly.config`、`anomaly.interop.signature`、`anomaly.scheduler`、`anomaly.ue5.names`、`anomaly.ue5.objects`；`anomaly.ui`、`anomaly.window`（可选） |
| **需要 Profile** | 通用对象与名称服务需要；FakeUID 专用签名和布局由插件自带 |

> [!NOTE]
> Custom UID 的 Manifest 当前声明 `builds: ["nte-*"]`，但插件内置的签名和布局只在已知游戏版本上验证过。宿主仅提供通用签名扫描、调度、对象快照和名称解析；插件会在运行时检查自己的签名、对象布局和 vtable，检查不通过时显示为不可用，不会强行写入。

## Camera Tools

| | |
| --- | --- |
| **ID** | `anomaly.local.nte.camera-tools` |
| **作用** | 保持角色跟随时增加视距，或切换为可移动的自由相机；可选让场景跟随相机加载。 |
| **依赖服务** | `anomaly.core`、`anomaly.config`、`anomaly.input`、`anomaly.ui`、`anomaly.localization`、`anomaly.interop.signature`、`anomaly.interop.hook` |
| **需要 Profile** | 否（相机签名和布局由插件自带，并在加载时校验） |

额外视距默认为 `0`，即完全使用游戏默认视距；插件不设置人为上限。自由相机和“场景跟随相机加载”默认关闭，激活键为 `F6`。启用该选项后，插件只在自由相机已激活时让场景按本地 PlayerController 的自由相机位置和旋转加载；关闭时完全保留游戏原始加载位置。

## WalletCollector

| | |
| --- | --- |
| **ID** | `anomaly.local.nte-interactbox-collector` |
| **作用** | 扫描当前地图的钱包刷新点，按目标数量规划路线，自动移动到各点并通过拾取服务确认钱包已收集。 |
| **依赖服务** | `anomaly.core`、`anomaly.ui`、`anomaly.localization`、`anomaly.nte.player`、`anomaly.nte.pickup`、`anomaly.interop.signature`、`anomaly.ue5.names`、`anomaly.ue5.objects`；`anomaly.nte.session`、`anomaly.nte.navigation`、`anomaly.nte.map-landmarks`、`anomaly.ue5.framework`（均为 V1，可选） |
| **需要 Profile** | 是；玩家、拾取、寻路和地图地标服务由活动 Profile 的 Feature Gate 提供，钱包点签名与 UE5 对象 / 数据表布局还会在插件运行时校验。 |

窗口中的 **目标钱包数量** 默认是 `10`，可在 `1` 到 `500` 之间调整。点击 **开始捡钱包** 后，插件会等待扫描完成，以玩家当前位置为起点规划路线，逐点移动、等待交互并验证拾取结果；寻路停滞时会尝试其他接近方向，拾取未确认时会自动重试，仍未确认的点会计入“跳过”。**停止** 会停止当前移动并清空未完成路线。

默认使用 **寻路捡钱包**。地图地标服务可用时，插件会利用它优化跨区路线；服务不可用时按常规寻路继续处理。

## 开发者模式调试插件

启用会话开发者模式后，插件列表还会显示 `Teleport Landmarks Probe`
（`anomaly.builtin.teleport-landmarks-probe`）。它枚举框架提供的全部可传送地标，逐项显示
`TeleportID`、所属世界、世界坐标、楼层、类型和覆盖目的坐标，并允许从选项中提交
`Normal` 或 `SellingIndulgences` 传送。该插件只消费
`anomaly.nte.map-landmarks`，不保存签名或偏移，也不自行扫描对象、解析 DataTable 或调用
UE `ProcessEvent`；关闭开发者模式后不会出现在已安装插件视图中，也不会执行传送请求。

### DLL Loader

| | |
| --- | --- |
| **ID** | `anomaly.builtin.dll-loader` |
| **作用** | 在插件启用或重载时加载指定的原生 DLL，并在插件停用或重载时释放该 DLL 的加载引用。 |
| **依赖服务** | `anomaly.config`、`anomaly.ui` |
| **需要 Profile** | 否 |

默认目标为 `dumper-7.dll`。把目标 DLL 放进 `Anomaly\plugins\DllLoader\` 后启用该插件即可加载；也可以在插件窗口输入包内相对路径或绝对路径。路径修改在渲染回调中只保存在内存，随后从 **Plugins** 页重载该插件，生命周期会先保存设置、释放旧 DLL，再加载新路径。将路径清空并重载可禁用 DLL 加载。

## 管理插件

在 **Plugins** 页你可以：

- 搜索 / 过滤 / 排序已安装插件。
- 启用、停用或重载单个插件。
- 查看插件状态、失败原因和回调耗时。
- 通过 **Open / Hide** 控制插件窗口；**Reload all** 显式触发全量重载。

下载安装第三方插件见[第三方插件](third-party-plugins.md)；自己写插件见[插件开发](../developer-guide/plugin-development.md)。
