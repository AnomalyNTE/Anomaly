# 快速上手

假设你已经按[安装与更新](installation.md)完成了代理安装，或者准备从启动器使用实时附加。本页从第一次进入游戏讲起。

## 1. 启动游戏

代理已经启用时，像平时一样启动游戏即可。使用实时附加时，要先退出已有的 `HTGame.exe`，再从 `AnomalyLauncher` 的 **实时附加** 页面点击 **启动并附加**。Runtime 进入游戏进程后会写入两份日志：

- `Anomaly\logs\anomaly-runtime.log` — Runtime 状态、PID、命名管道名。
- `Anomaly\anomaly-platform.log` — 平台与插件日志。

如果游戏启动后这些日志出现且记录了 PID，说明 Runtime 已成功注入。

## 2. 呼出管理界面

按 **`Insert`** 键呼出 / 折叠管理界面。它是一个可拖动、可缩放的 ImGui 浮动窗口，首次显示时居中为 `1040×700`，位置与尺寸保存在 `Anomaly\anomaly-imgui.ini`。

主管理界面固定为三个主导航：

| 页面 | 内容 |
| --- | --- |
| **Plugins** | 管理已安装插件，浏览和更新第三方插件，并维护插件源。 |
| **Diagnostics** | 查看 Runtime 状态；**Developer** 区域中还有服务、Hook、内存和 NTE 兼容性信息。 |
| **Settings** | 平台与插件设置（语言、切换键等）。 |

> [!NOTE]
> 主管理窗口没有关闭按钮，以免你失去再次打开平台的入口。`Insert` 会折叠 / 展开未锁定的菜单，并兜底恢复异常关闭的主窗口；标题栏的锁按钮可让单个窗口保持展开。

## 3. 启用内建插件

在 **Plugins** 页可以看到随运行包安装的内建插件。选择一个插件即可启用 / 停用、重载或查看其状态原因。

首次安装时所有插件均为关闭状态，不会随 Runtime 自动加载；请按需逐个启用。启用带有必需依赖的插件时，Runtime 会同时启用其依赖项。

- 插件窗口通过管理界面的 **Open / Hide** 控制；点插件窗口的 X 关闭后可从这里重新打开。
- 每个插件是独立的目录包；根级 DLL 与没有 Manifest 的目录不会被加载。

各内建插件的作用见[内建插件](built-in-plugins.md)。

> [!TIP]
> 坐标、实体 ESP 和传送都依赖 Profile。如果它们显示为不可用或降级，请先查看 [NTE Build Profile](nte-profiles.md) 中的符号和 Feature 状态。

## 4. 安装第三方插件

打开 **Plugins > 可用**，找到插件后点击 **安装**。安装完成后，它会出现在 **已安装** 中，并保持关闭状态；看过说明和状态后再手动启用。

插件有新版本时会出现在 **更新**。要添加或停用插件源，打开 **第三方插件**。详细步骤和安全提醒见[第三方插件](third-party-plugins.md)。

## 5. 切回游戏

切回游戏（隐藏或折叠界面）后，所有 Anomaly 窗口都不再接收鼠标，菜单折叠也不影响插件的前景绘制（例如 ESP 覆盖层仍会渲染）。

## 6. （可选）用 CLI 做诊断

Runtime 暴露一个命名管道诊断协议。控制端 `anomaly-cli.exe` 可放在任意目录，通过 PID 连接：

```powershell
.\anomaly-cli.exe --pid <PID> status
.\anomaly-cli.exe --pid <PID> modules
```

完整命令见[诊断 CLI](diagnostics-cli.md)。

## 下一步

- [配置参考](configuration.md) — 修改切换键、语言、性能项
- [内建插件](built-in-plugins.md) — 每个内建插件做什么
- [第三方插件](third-party-plugins.md) — 下载、更新和管理插件源
- [故障排查与 FAQ](troubleshooting.md) — 界面 / 插件 / Feature 常见问题
