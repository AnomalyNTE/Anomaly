# NTE Combat Tracker

Anomaly 的内建战斗统计插件：只读实时伤害/治疗统计、独立 Overlay 进程，以及落在本地的
高保真 JSONL 战斗日志与单场 JSON 导出。

插件 ID `local.nte.combat-stats` 保持不变，已安装的用户升级时仍按同一身份更新。

> [!NOTE]
> 这是内建插件，源码就在本仓库。它的构建信息登记在仓库根 `CMakeLists.txt` 的
> `if(ANOMALY_BUILD_BUILTIN_PLUGINS)` 块内；本目录**不放**自己的 `CMakeLists.txt`
> （那是 SDK 独立消费者的入口）。

## 目录结构

```text
plugins/NteCombatTracker/
  manifest.json           插件 manifest（name/description 用英文回退）
  locales/zh-CN.json      中文目录（window.title / plugin.description）
  src/                    插件 DLL、Overlay 与采集引擎的源码
  tests/                  15 个离线夹具，随 -DANOMALY_BUILD_TEST_FIXTURES=ON 构建
  assets/dict/            运行时名称字典（29 个角色文件 + 623 条怪物 + 属性配色）
  data/                   字典生成器的输入源（DataTable 快照 + enemies.json）
  script/                 字典生成与自检脚本
  cmake/plugin_version.hpp.in
```

`assets/dict/` 由 `anomaly_add_plugin` 自动镜像进包目录并随 GameRuntime 组件安装；
`data/` 只在生成字典时使用，不进包。

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

## 角色字典维护

`assets/dict/character/*.json` 与 `assets/dict/monster.json` 是从 `data/` 派生的快照，
**运行时只读 `assets/dict`**，不读 `data/`。

```powershell
pwsh -NoProfile -File plugins/NteCombatTracker/script/build-dict.ps1 -Write
pwsh -NoProfile -File plugins/NteCombatTracker/script/build-dict.ps1 -Check
pwsh -NoProfile -File plugins/NteCombatTracker/script/test-build-dict.ps1
```

更新 `data/datatable/` 或 `data/enemies.json` 后重新生成并提交快照。生成器输出固定为
UTF-8 无 BOM、两空格缩进、不含生成时间，连续生成不产生无意义差异。

## 实现要点

- **双进程**：插件采集并通过宿主 `anomaly.websocket` 发布 JSON 快照；Overlay 以 WinHTTP
  WebSocket 客户端连接 `127.0.0.1:<宿主端口>`，用 Direct2D 自绘，不受宿主 ImGui 能力限制。
- **CRT 依赖**：插件与 Overlay 都以 `/MD` 构建，包内不带 CRT 副本。`LaunchOverlay` 先从
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

