# 用户文档

这里写的是 Anomaly 的日常用法：怎么安装、去哪找插件、怎样更新，以及出问题时先看什么。如果你要从源码构建或自己写插件，请转到[开发者文档](../developer-guide/README.md)。

## 阅读顺序

1. [安装与更新](installation.md) — 下载正确的运行包，用启动器完成首次安装、更新或实时附加。
2. [快速上手](quickstart.md) — 第一次启动、打开界面并启用插件。
3. [内建插件](built-in-plugins.md) — 随运行包安装的插件分别做什么。
4. [第三方插件](third-party-plugins.md) — 在游戏内下载、更新、卸载插件，以及管理插件源。
5. [故障排查与 FAQ](troubleshooting.md) — 界面不出现、下载失败、插件不加载等常见问题。
6. [配置参考](configuration.md) — 修改切换键、语言、Profile 目录和插件源。
7. [诊断 CLI](diagnostics-cli.md) — 用 `anomaly-cli` 查看 Runtime、Profile 和内存状态。
8. [NTE Build Profile](nte-profiles.md) — 了解依赖游戏结构的功能为什么可能暂时不可用。

## 术语速查

| 术语 | 含义 |
| --- | --- |
| **Runtime / Core** | 注入到游戏进程内的 `Anomaly.Core.dll`，承载全部平台能力。 |
| **代理** | 根目录的 `dwmapi.dll`，转发系统导出并引导启动 Core。 |
| **平台 / Platform** | Runtime 内的宿主 UI 与插件管理层。 |
| **Profile** | 游戏符号、结构偏移和 Feature 依赖的集合。 |
| **Feature** | 依赖已验证符号的能力（如玩家坐标、实体快照）；不满足时按 Feature 降级为不可用。 |
| **插件（plugin）** | 一个目录包，含 `manifest.json` 与 `plugin.dll`，由 Runtime 加载并可热重载。 |
| **插件源** | 一个 `pluginmaster.json` 地址，提供可下载的第三方插件列表。 |

> 使用前请阅读根目录 [README](../../README.md) 中的免责声明。
