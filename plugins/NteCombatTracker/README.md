# NTE Combat Tracker

Anomaly 的内建战斗统计插件：只读实时伤害/治疗统计、独立 Overlay 进程，以及落在本地的
高保真 JSONL 战斗日志与单场 JSON 导出。

插件 ID 为 `shikari.nte-combat-tracker`，遵循上游的 `<作者>.<插件名>` 规则。从独立仓库
迁移的用户，其旧 id `local.nte.nte-combat-tracker` 下的配置与状态由
`tools/migrate_plugin_ids` 迁移。

> [!NOTE]
> 这是内建插件，源码就在本仓库。它的构建信息登记在仓库根 `CMakeLists.txt` 的
> `if(ANOMALY_BUILD_BUILTIN_PLUGINS)` 块内；本目录**不放**自己的 `CMakeLists.txt`
> （那是 SDK 独立消费者的入口）。

## 目录结构

```text
plugins/NteCombatTracker/
  manifest.json           插件 manifest（name/description 用英文回退）
  locales/zh-CN.json      中文目录（window.title / plugin.description）
  profile/                compatibility.profile.json：签名、布局偏移与虚表槽位
  src/                    插件 DLL、Overlay 与采集引擎的源码
  tests/                  15 个离线夹具，随 -DANOMALY_BUILD_TEST_FIXTURES=ON 构建
  assets/                 仅属性配色表（角色属性在运行时从 DT_Character 读取）
  cmake/plugin_version.hpp.in
```

`assets/` 与 `profile/` 由 `anomaly_add_plugin` 自动镜像进包目录并随 GameRuntime 组件安装。
`profile/compatibility.profile.json` 是唯一的兼容数据来源：签名、入口字节、对象/反射/DataTable
偏移、虚表槽位与记录步长都在那里，代码只保留 `struct_size`/指令解码与范围、对齐校验。宿主在
Lifecycle 阶段通过 `anomaly.profile` 校验并加载，缺项或签名失配时停用 UID 探测与属性配色，
不回退到代码内偏移。

## 名称来源

**所有用户可见名称都来自宿主 ABI**，插件不再维护本地名称字典：

| 数据 | ABI |
| --- | --- |
| 伤害 / 治疗 / Buff 事件名 | `anomaly.nte.combat` → `event_name_utf8` |
| 参与者名 | `anomaly.nte.combat` → `participant_display_name_utf8` |
| 技能名（开发者页） | `anomaly.nte.skills` → `ability_display_name_utf8` |

宿主负责 GE→GA→文本、怪物配置→显示名的映射，并在 Game 更新中延后补全；未就绪时
`NOT_FOUND`，插件退回对象路径或 `#<name_id>` 占位符，并按 64 个事件的节奏重试。

对象路径只作临时标签、**不进名称缓存**，否则真名到位后会被路径长期占位。

`assets/attribute_colors.json` 只提供**行配色**：六个属性的代表色，不参与任何名称解析。

## 角色属性来源

角色的属性（灵/相/暗/光/魂/咒）**不在本地字典里**，运行时从游戏自己的
`DT_Character` 行读取：

| 步骤 | 途径 |
| --- | --- |
| 定位表对象 | `anomaly.ue5.objects` → `find_exact("/Game/DataTable/Character/DT_Character.DT_Character")` |
| 句柄 → 对象地址 | 按 Profile 的 `kGObjectsPattern` 解析注册表（`anomaly.profile` + `anomaly.interop.signature`），再按 Profile 布局读注册表 |
| 逐行读取 | `anomaly.core` → `read_memory` 遍历 `UDataTable::RowMap` |
| 字段偏移 | 走行结构体的 `ChildProperties` 链（含 `SuperStruct`）解析 `CharacterElementType`，偏移来自 Profile |
| 行 ↔ 角色 | 角色对象路径里的 `player_003` 对应行键 `1003` |

`ElementData.CharacterElementType` 是 1 字节枚举，值映射到
`ECharacterElementType` 的后缀（`CHAOS`/`COSMOS`/`INCANTATION`/`LAKSHANA`/`NATURE`/`PSYCHE`）。
**宿主 ABI 没有属性查询接口**，其反射读（`entities`/`actors`）只覆盖 `bool`、`FName` 和
`USceneComponent*` 三种类型，读不到这个枚举——所以走 DataTable。

属性表读不到时（表未加载、签名失效、无 `memory-read` 授权）不报错也不阻塞：该行退回
稳定的身份配色，其余功能照常。插件启动时读一次，不逐帧重读。

`assets/attribute_colors.json` 是这六种属性的代表色，键是上面的枚举后缀，不是中文名。

新增角色**不需要改插件**——表里加了行就能读到。只有游戏新增属性种类时才需要补配色。

## 构建与验证

```powershell
cmake --preset windows-vs2022 -DANOMALY_BUILD_TEST_FIXTURES=ON
cmake --build --preset windows-relwithdebinfo --parallel
ctest --test-dir .build\windows-vs2022 -C RelWithDebInfo -R nte_
```

包产物位于 `.build/windows-vs2022/bin/RelWithDebInfo/builtin-plugins/NteCombatTracker/`，
其中 `NteCombatTracker.exe` 由根 `CMakeLists.txt` 的 POST_BUILD 规则拷入——`anomaly_add_plugin`
只处理 `locales/` 与 `assets/`，不会自己带上额外可执行文件。

插件文案门禁：

```powershell
pwsh -NoProfile -File tools/check_plugin_i18n.ps1 -Plugin NteCombatTracker
```

## 实现要点

- **兼容数据**：签名、入口字节与布局只在 `profile/compatibility.profile.json`；离线夹具也从
  同一份 Profile 派生，生产代码与测试都不保留第二份数值副本。
- **双进程**：插件采集并通过宿主 `anomaly.websocket` 发布 JSON 快照；Overlay 以 WinHTTP
  WebSocket 客户端连接 `127.0.0.1:<宿主端口>`，用 Direct2D 自绘，不受宿主 ImGui 能力限制。- **CRT 依赖**：插件与 Overlay 都以 `/MD` 构建，包内不带 CRT 副本。`LaunchOverlay` 先从
  包目录把 CRT 模块解析进当前进程，子进程再继承，避免默认搜索顺序造成的加载器弹窗。
- **日志**：schema v3，`events-NNNNNN.jsonl` 按 1024 事件 / 1 MiB 分段，`index.json` 记录
  逐段 SHA-256；所有写入都是临时文件 + `MoveFileExW`。
- **账号隔离**：只读抓取 `TextBlock_RoleID` 控件文本得到 UID，按 UID 分目录；识别不到时写
  `_unknown`。

## 已知限制

- 仅支持 Windows x64、NTE 与 Plugin API 1；游戏或 SDK 更新可能暂时中断采集。
- Runtime 目前不提供治疗事件，`healing_capture_status` 固定 `unavailable`，治疗/受疗恒为 0，
  **不能**读作真实零值。
- 时间戳是客户端接收时间，不是服务器判定时间。
- 本地日志默认不限量，磁盘与账号标识的处置由使用者负责。
- 不支持队友/多人统计，只统计自己操控的角色。

