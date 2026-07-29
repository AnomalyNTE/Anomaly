# 故障排查与 FAQ

按现象查找。日志是最重要的排查入口：

- `Anomaly\logs\anomaly-runtime.log` — Runtime 与管道服务日志（PID、状态、管道名）。
- `Anomaly\anomaly-platform.log` — 平台与插件日志。

## 安装 / 启动器

### 选择目录后仍然不能安装

- 选择的是**直接包含 `HTGame.exe` 的文件夹**，不是 `Neverness To Everness` 安装根目录。
- `AnomalyLauncher.exe`、`dwmapi.dll` 和 `Anomaly\Anomaly.Core.dll` 必须来自同一个完整解压的 Runtime 包。只移动启动器会让安装源不可用。
- 更新前先退出游戏，避免旧文件仍被进程占用。

### 启动器显示“存在冲突”

游戏目录里可能已经有其他工具的 `dwmapi.dll`，或者 `dwmapi.dll` 与 `dwmapi.dll.disabled` 同时存在。启动器不会替你覆盖或删除这些文件。先确认文件属于哪个工具，再决定保留哪一个；Anomaly 与 RE-UE4SS 不能共用同名代理。

### “启动并附加”是灰色的

实时附加只处理启动器这次新建的游戏进程。先退出所有 `HTGame.exe`，确认页面已经找到 `NTELauncher.exe` 并显示可用的 Runtime，再点击 **启动并附加**。

## 界面 / 注入

### 按 `Insert` 没反应，界面不出现

1. 确认 Runtime 已注入：上述日志文件是否生成并记录了 PID？
2. 确认切换键：`anomaly.ini` 的 `[Platform] ToggleKey`（Win32 虚拟键码，默认 `45` = `Insert`）。
3. 确认平台已启用：`[Platform]` 中应为 `Enabled=1` 和 `Embedded=1`。`Visible=0` 是正常默认值，只表示启动时先隐藏界面。
4. 确认 `dwmapi.dll` 放在**游戏主 EXE 所在目录**，且没有被重命名为 `dwmapi.dll.disabled`。
5. 检查是否与 RE-UE4SS 冲突（见下）。

### 日志没有生成 / Runtime 没注入

- 确认使用的是 Anomaly 的 `dwmapi.dll`（不是系统的）。
- 代理模式下，游戏必须实际加载 `dwmapi.dll`。
- 实时附加：**若游戏从创建起即限制进程句柄，请改用代理安装**。

### 和 RE-UE4SS 冲突

Anomaly 的代理与 RE-UE4SS 的引导 DLL **同名**（都是 `dwmapi.dll`），不能共用同一目录。二者只能存在一个。Anomaly 不加载 `UE4SS.dll`。

## 第三方插件下载与更新

### “可用”页面是空的

- 打开 **Plugins > 第三方插件**，确认总开关和至少一个插件源已启用，并且地址使用 HTTPS。
- 点击“应用”后回到 **可用**，再点右上角刷新。
- 插件源离线时可能只剩上一次缓存的列表；第一次使用且没有缓存时，页面会保持不可用。
- 某个插件没有出现，也可能是它在 `pluginmaster.json` 中缺少必填字段，或 ID / SemVer 格式错误。

### 安装或更新失败

- 先看插件行下方的失败原因，再看 `Anomaly\anomaly-platform.log`。
- `plugin package URL must use HTTPS`：下载地址不是 HTTPS，或插件源给了无效地址。
- `manifest id/version does not match`：插件列表与 ZIP 内的 Manifest 没有一起更新。
- `package has no manifest.json at its root`：ZIP 多套了一层目录。
- `manifest plugin API does not match` / `not published for this game`：插件不支持当前 Runtime 或游戏。
- 提示缺少依赖时，到 **可用** 中单独安装依赖插件；安装器不会自动下载依赖。

完整使用方法见[第三方插件](third-party-plugins.md)。

## 插件

### 插件不加载

- 插件必须是**含 `manifest.json` 的目录包**。根级 DLL 与没有 Manifest 的目录不会被加载。
- 查看 Manifest 是否通过 schema 校验，并确认所需服务和 capability 声明完整。
- 查看 **Plugins** 页对应插件的**状态原因**，常见拒绝原因：
  - **unknown capability** — Manifest 声明了未知 capability。
  - **required service missing mapping / capability** — 某个非可选 required service 没有对应 capability 声明。
  - **DLL 预检冲突** — 与已加载模块同名，或存在无法隔离的 native 依赖冲突。

capability 与 service 的对应关系见[插件开发](../developer-guide/plugin-development.md)与 [Manifest 与 capability](../api-reference/manifest-and-capabilities.md)。

### 插件加载了，但某些功能不可用 / 灰掉

多半是 [Feature 降级](nte-profiles.md#feature-降级)：活动 Profile 缺少所需声明，或符号、布局、ABI 校验没有通过。这时插件仍能加载，只是相关功能保持 `unavailable`。

用 CLI 的 `ue` 命令或 **Diagnostics > Developer > NTE Compatibility** 查看 Feature Matrix。

### 重载插件后行为异常

- 一个包变化时只会重载该包**及依赖它的下游包**；如需全量重载，用 **Reload all**。
- 若某代际 drain 超时被 quarantine，其 DLL 会保持映射到进程退出。这是有意的安全设计（执行已被 unmap 的代码更危险）。

## CLI / 诊断

### `anomaly-cli` 连不上

- 确认 `--pid` 是**游戏进程**的 PID（见 `anomaly-runtime.log`）。
- 管道受当前用户 ACL 限制：CLI 必须以**同一 Windows 用户**运行。
- 确认 `[Analyzer] PipePrefix` 与预期一致。

### 命令返回 error

响应中的 `error.code` / `error.message` 是稳定的错误标识。内存写入类命令要求目标地址页面可写 / 存在。

## 配置

### 改了 `Language` 没变化

`Language` 修改后需要**重启对应进程**才生效。取值只能是 `auto`、`en-US`、`zh-CN`。

### 找不到插件的持久化设置

插件通过 `anomaly.config` 保存的 JSON 设置位于 `Anomaly\config\plugins\`；全局启用状态位于 `Anomaly\config\plugin-enablement.json`。

## 崩溃

- 未处理异常会生成 `MiniDumpNormal` 与旁路 metadata。
- `AnomalyCrashCoordinator.exe` 会区分正常退出和启动故障。同一类故障反复发生时，它会逐步启用最小 Core、暂停 Profile 覆盖、停用第三方插件或回退待定 Runtime。当前恢复状态可在 Runtime 诊断中查看。

> [!CAUTION]
> 诊断包、崩溃转储与 Profile 数据可能含敏感信息，分享前请审查并脱敏。

## 还没解决？

- 复查[安装与更新](installation.md)、[第三方插件](third-party-plugins.md)与[配置参考](configuration.md)。
- 开发 / 构建相关问题见[开发者文档](../developer-guide/README.md)。
