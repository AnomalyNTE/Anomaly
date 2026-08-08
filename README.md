<div align="center">

<img src="./logo.png" alt="Anomaly" width="256" height="256" />

# Anomaly 异象

**稳定、灵活、开放的《异环》插件平台**

qq交流群: 1037114140

<p align="center">
  <a href="docs/user-guide/README.md">用户文档</a> ·
  <a href="docs/user-guide/third-party-plugins.md">第三方插件</a> ·
  <a href="docs/developer-guide/README.md">开发者文档</a> ·
  <a href="docs/api-reference/README.md">API 参考</a> ·
  <a href="docs/user-guide/quickstart.md">快速上手</a>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/platform-Windows%20x64-0078D6?logo=windows" alt="Platform" />
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus" alt="C++20" />
  <img src="https://img.shields.io/badge/plugin%20ABI-v1-success" alt="Plugin ABI v1" />
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-AGPL--3.0--only-blue" alt="License" /></a>
</p>

</div>

---

## ⚠️ 免责声明与风险提示

> [!NOTE]
> Anomaly 是**面向研究、诊断与插件开发**的工具，不得用于作弊行为。
> 内存读写、hook、patch、实体扫描会修改或读取其他进程的运行状态，可能导致游戏崩溃、数据损坏或账号封禁。
> 日志、崩溃转储、Profile 数据与诊断包**可能包含敏感信息**，分享前请审查并脱敏。

> [!CAUTION]
>
> 根据[《异环》公平游戏宣言](https://yh.wanmei.com/news/gamebroad/20260202/260701.html)：
>
> 严禁使用任何第三方工具破坏游戏公平性。我们将严厉打击使用外挂、加速器、作弊软件、宏脚本等非法工具的行为，这些行为包括但不限于自动挂机、技能加速、无敌模式、瞬移、修改游戏数据等操作。一经查实，我们将视违规严重程度及违规次数，采取包括但不限于扣除违规收益、冻结游戏账号、永久封禁游戏账号等措施。
>
> **您应充分了解并自愿承担使用本工具可能带来的所有风险。**

## 🧾 开源许可

本项目以 [**GNU Affero General Public License v3.0（AGPL-3.0-only）**](LICENSE) 分发，第三方组件及其各自的许可证见 [`NOTICE`](NOTICE) 与 `third_party/licenses/`。

## ✨ 功能介绍

- 🚀 **安装与更新**：解压 Runtime，打开启动器，选中 `HTGame.exe` 所在目录即可安装；后续版本也能直接更新。
- 🪟 **游戏内管理界面**：按 `Insert` 呼出中文或英文界面，统一管理插件的启用、停用、重载和设置。
- 🌐 **第三方插件下载**：在 **Plugins > 可用** 中浏览和安装社区插件，在 **更新** 中升级，也可以自行添加可信插件源。
- 🔌 **实用内建插件**：随包提供坐标显示、实体 ESP、粉爪大劫案信息、自定义 UID 与相机工具等插件，全部默认关闭，按需启用。
- 🎥 **相机工具**：Camera Tools 将视距修改与自由相机整合到同一套相机 Hook 中，避免两个相机插件同时启用时发生 Hook 冲突。
- 🧭 **状态与排错**：插件不兼容、依赖缺失或 Profile 未就绪时会直接显示原因，详细状态可在 **Diagnostics** 和日志中查看。
- 🧩 **开放插件框架**：提供纯 C ABI v1 SDK 和统一 V1 服务，插件工程无需依赖 Runtime 源码或 C++ ABI。
- 🔄 **完整插件生命周期**：支持 Manifest、SemVer、依赖与 capability 校验、包校验、文件监控热重载和失败回滚。
- 🧬 **UE5 / NTE 适配**：提供经过校验的 UE5 符号解析，以及会话、玩家、相机、实体、地图地标和传送等高层服务；条件不满足时按功能降级。
- 🔍 **内存分析与开发工具**：支持查看进程模块、PE 节区和内存区域，并提供字节签名扫描、离线 TestHost 等开发工具。
- 🛡️ **故障隔离**：单个插件异常后会被隔离；新版本加载失败时自动回滚到上一代。

### 🔌 内建插件

运行包默认安装以下插件（详见[内建插件](docs/user-guide/built-in-plugins.md)）：

| 插件 | 作用 |
| --- | --- |
| **Coordinate Display** | 显示玩家坐标、会话状态与采样指标 |
| **Entity ESP** | 绘制实体边界框与标签 |
| **Pink Paw Heist ESP** | 显示粉爪大劫案的战利品与撤离点，可按价值筛选 |
| **Custom UID** | 自定义客户端界面上显示的 UID |
| **Camera Tools** | 增加视距和自由相机视角 |

### 🌐 第三方插件

按 `Insert` 打开 **Plugins**，就能完成第三方插件的日常管理：

| 页面 | 用来做什么 |
| --- | --- |
| **可用** | 搜索插件并下载安装 |
| **更新** | 查看已安装插件的新版本 |
| **已安装** | 启用、停用、重载或卸载插件 |
| **第三方插件** | 添加、停用或移除插件源 |

运行包预置了一个已启用的插件源，实际地址以 `Anomaly\plugin-repositories.json` 为准。也可以在界面中添加自己信任的 HTTPS 插件源。完整步骤见[第三方插件](docs/user-guide/third-party-plugins.md)。

> [!WARNING]
> 第三方插件是会在游戏进程中运行的原生 DLL，不是受限脚本。插件列表和插件包目前没有发布者签名；Anomaly 会检查下载地址、ZIP 路径、Manifest、版本、游戏与 API 是否匹配，但这些检查不能证明插件本身安全。只安装你信任的来源和作者提供的插件。

## 🚀 快速上手

> [!NOTE]
> 普通用户无需构建源码。下载 `Anomaly-<版本号>-runtime.zip` 并完整解压，运行其中的 `AnomalyLauncher.exe`，在 **代理安装** 页面选择 `HTGame.exe` 所在目录并点击 **安装**。

1. 下载并安装 Runtime，具体步骤见[安装与更新](docs/user-guide/installation.md)。
2. 像平时一样启动游戏，按 `Insert` 呼出管理界面。
3. 使用内建插件：打开 **Plugins > 已安装**，选中插件后启用。
4. 安装社区插件：打开 **Plugins > 可用**，点击 **安装**；完成后回到 **已安装** 启用它。
5. 插件有新版本时，在 **Plugins > 更新** 中升级。

从源码构建（开发者）：

```powershell
.\build.cmd
```

`build.cmd` 是唯一受支持构建路径（CMake Preset `windows-vs2022` + `windows-relwithdebinfo`）的薄包装器，配置 → 构建后生成可直接部署的运行包。详见[从源码构建](docs/developer-guide/building.md)。

## 📚 文档

| 文档集 | 面向 | 内容 |
| --- | --- | --- |
| [📘 用户文档](docs/user-guide/README.md) | 使用者 | 安装、内建与第三方插件、更新、配置和故障排查 |
| [🛠️ 开发者文档](docs/developer-guide/README.md) | 插件作者与框架贡献者 | 插件开发与发布、从源码构建、架构和贡献流程 |
| [📑 API 参考](docs/api-reference/README.md) | 插件作者 | 完整的纯 C ABI v1：生命周期、全部服务表、Manifest 与 capability |

项目级参考：[架构概览](docs/developer-guide/architecture.md)（模块边界与依赖方向）与 [`AGENTS.md`](AGENTS.md)（协作约定）。

## ❓ 常见问题

> [!TIP]
> 更多问题见[故障排查与 FAQ](docs/user-guide/troubleshooting.md)。

- **按 `Insert` 没反应？** 切换键可在 `anomaly.ini` 的 `[Platform] ToggleKey` 修改（Win32 虚拟键码，默认 `45` = `VK_INSERT`）。
- **“可用”里没有插件？** 到 **Plugins > 第三方插件** 确认总开关和插件源都已启用，再回到 **可用** 点击刷新。详见[第三方插件](docs/user-guide/third-party-plugins.md)。
- **第三方插件安装失败？** 常见原因是下载地址不可用、ZIP 结构不对，或插件 ID、版本、游戏和 API 与列表不一致。界面会显示失败原因，详细记录在 `Anomaly\anomaly-platform.log`。
- **插件加载失败？** 确认它是含 `manifest.json` 的目录包；根级 DLL 与无 Manifest 的目录不会被加载。
- **坐标 / 实体等功能显示不可用？** 通常是活动 Profile 中缺少所需签名，或符号、布局校验未通过。见 [NTE Build Profile](docs/user-guide/nte-profiles.md)。
- **和 RE-UE4SS 冲突吗？** 代理文件名同为 `dwmapi.dll`，两者不能共用同一目录；Anomaly 是独立入口，不加载 `UE4SS.dll`。

## 💡 注意事项

- 仅支持 **Windows x64**。运行插件只需安装后的 SDK，无需 Runtime 源码。
- 实时进程工具（`write` / `patch` / `protect` / `alloc` / `free`）会修改目标进程，不是普通使用流程，**只能用于你自己的测试进程**。
- 主仓库只有**一条**受支持的构建路径，请勿新建临时 CMake / NMake 构建树。

## 💻 开发指南

- [从源码构建](docs/developer-guide/building.md) — 环境、`build.cmd`、验证与发布打包
- [架构概览](docs/developer-guide/architecture.md) — 线程域、ServiceGraph、所有权与 ABI 边界
- [插件开发](docs/developer-guide/plugin-development.md) — SDK、Manifest、capability、生命周期与热重载
- [发布第三方插件](docs/developer-guide/plugin-distribution.md) — 打包 ZIP、配置在线插件源 JSON、发布与验证
- [贡献指南](docs/developer-guide/contributing.md) — 工作流、提交规范、代码边界与验证门禁

新建独立插件仓库可使用 [Anomaly 插件模板](https://github.com/AnomalyNTE/Anomaly-Plugin-Template)；仓库 [`examples/`](examples/README.md) 内另有四个独立 SDK 示例。

## ❤️ 鸣谢

Anomaly 在运行时或分发包中使用了以下开源组件：

- [Dear ImGui](https://github.com/ocornut/imgui) — 内嵌图形界面
- [MinHook](https://github.com/TsudaKageyu/minhook) — 函数 hook 后端
- [nlohmann/json](https://github.com/nlohmann/json) 与 [JSON schema validator](https://github.com/pboettch/json-schema-validator) — JSON 与 Schema 校验
- [Noto Sans CJK](https://github.com/notofonts/noto-cjk) — 中英文 UI 字体
- [Dalamud](https://github.com/goatcorp/Dalamud) — 灵感来源
