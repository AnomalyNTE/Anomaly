# 从源码构建

## 环境要求

- **Windows 10 / 11 x64**
- **Visual Studio 2022** C++ 生成工具（MSVC）
- **Windows SDK**
- **CMake 3.22+**
- **PowerShell 7+（`pwsh`）** — `tools\*.ps1`、测试工具与发布流程需要

## 唯一受支持的构建路径

> [!IMPORTANT]
> 主仓库只有**一套**受支持的构建路径：CMake Preset `windows-vs2022` 配置 + `windows-relwithdebinfo` 构建。CI、发布与本机构建都走这条路径。**不要**新建 `cmake -S/-B`、NMake、Ninja 或按阶段命名的构建树。全部构建配置以 [`CMakePresets.json`](../../CMakePresets.json) 为准。

### 一条命令

`build.cmd` 是下面命令序列的薄包装器，默认只构建 Runtime、Tools 与 SDK 所需的 target，并在构建完成后生成 GameRuntime 安装树：

```powershell
.\build.cmd
```

| 参数 | 作用 |
| --- | --- |
| （无参数） | 构建 Runtime、Tools、SDK target，并安装可直接部署的运行包 |
| `fixtures` | 额外构建开发夹具（`anomaly-platform-preview`、`anomaly-render-fixture`、`anomaly-d3d11-fixture`、BetterPose 与自动战斗测试程序） |
| `probes` | 额外构建诊断探针插件包（传输 trace、BidKing 探针） |
| `symbols` | 额外生成 linker PDB 并暂存 Symbols 组件，只用于本地调试 |
| `package` | 额外把 Tools、SDK（开启 `symbols` 时含 Symbols）安装到 `.build\windows-vs2022\package`，与发布打包一致 |

参数可以组合，例如 `.\build.cmd fixtures package`。开发夹具、诊断探针与 PDB 都不属于发布的
三个组件，默认不构建，只有显式开启时才会出现在构建树中。

### 手动执行（等价）

```powershell
cmake --preset windows-vs2022
cmake --build --preset windows-relwithdebinfo --parallel
cmake --install .build\windows-vs2022 --config RelWithDebInfo `
  --prefix .build\windows-vs2022\game-package --component GameRuntime
```

可选开关只在需要时于 configure 阶段显式开启：
`-DANOMALY_BUILD_TEST_FIXTURES=ON`、`-DANOMALY_BUILD_DIAGNOSTIC_PROBES=ON`、
`-DANOMALY_BUILD_SYMBOLS=ON`。

### 产物位置

| 内容 | 路径 |
| --- | --- |
| 构建出的工具（CLI、验证工具） | `.build\windows-vs2022\bin\RelWithDebInfo` |
| 可直接部署的干净运行包 | `.build\windows-vs2022\game-package` |
| 发布组件暂存目录（`build.cmd package`） | `.build\windows-vs2022\package\{runtime,tools,sdk}` |

运行包结构见[安装与更新](../user-guide/installation.md)。CLI 与验证工具保留在 `bin`，不会进入游戏运行包。

## 预览界面（无游戏）

`anomaly-platform-preview.exe` 属于开发夹具，需要先开启 `ANOMALY_BUILD_TEST_FIXTURES`
（`.\build.cmd fixtures`）。开启后直接运行它，可以在独立进程通过与游戏内相同的 Platform 与
PluginManager 路径预览和调试界面与插件。

## 发布打包

正式组件包由同一构建树生成：

```powershell
pwsh -NoProfile -File .\tools\package_release.ps1 `
  -BuildDirectory .build\windows-vs2022 `
  -Version 1.0.0 `
  -OutputDirectory .build\release\1.0.0
```

输出确定性组装的三个 ZIP 与校验文件：

| 组件 ZIP | 内容 |
| --- | --- |
| **Runtime** | 代理、Core、配置、bundled Profile、17 个内建插件包与 1 个示例插件包（NteCombatDemo） |
| **SDK** | 头文件、CMake 包、五个示例（源码 + 独立 CMake 工程） |
| **Tools** | 六个正式命令行工具 |

以及 `SHA256SUMS.txt`、`release-manifest.json` 与三份 SPDX 2.3 `sbom/*.spdx.json`。
tag workflow 发布 Runtime、Tools、SDK 三个 ZIP，并为这三个归档生成 GitHub build provenance
attestation。PDB 不属于发布内容：默认构建不生成 PDB，需要本地调试时用 `build.cmd symbols`。

## 命令行工具

`Tools` 组件包含六个正式工具：

| 工具 | 用途 |
| --- | --- |
| `anomaly-cli.exe` | 诊断客户端（命名管道） |
| `anomaly-inspect.exe` | 离线检查客户端 |
| `anomaly-plugin.exe` | 插件包校验与组装（`validate` / `pack`） |
| `anomaly-profile.exe` | Build / Profile 校验与离线 `fingerprint` |
| `anomaly-abi-snapshot.exe` | ABI 基线生成与校验 |
| `anomaly-test-host.exe` | 无游戏的插件 fixture 宿主 |

`AnomalyLauncher.exe` 与 `AnomalyCrashCoordinator.exe` 随 Runtime 分发；`anomaly-render-fixture.exe` 与 `anomaly-platform-preview.exe` 属于开发夹具，只在 `build.cmd fixtures` 之后出现在构建目录中。

## 相关

- [架构概览](architecture.md)
- [贡献指南](contributing.md)
- [插件开发](plugin-development.md)
