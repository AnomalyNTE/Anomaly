# 从源码构建

## 环境要求

- **Windows 10 / 11 x64**
- **Visual Studio 2022** C++ 生成工具（MSVC）
- **Windows SDK**
- **CMake 3.22+**
- **PowerShell 7+（`pwsh`）** — `tools\*.ps1`、测试工具与发布流程需要

## 唯一受支持的构建路径

> [!IMPORTANT]
> 主仓库只有**一套**受支持的构建路径：CMake Preset `windows-vs2022` 配置 + `windows-relwithdebinfo` 构建 / 测试。CI、发布与本机构建都走这条路径。**不要**新建 `cmake -S/-B`、NMake、Ninja 或按阶段命名的构建树。全部构建配置以 [`CMakePresets.json`](../../CMakePresets.json) 为准。

### 一条命令

`build.cmd` 是下面命令序列的薄包装器，会在测试通过后生成 GameRuntime 安装树：

```powershell
.\build.cmd
```

### 手动执行（等价）

```powershell
cmake --preset windows-vs2022
cmake --build --preset windows-relwithdebinfo --parallel
ctest --preset windows-relwithdebinfo
cmake --install .build\windows-vs2022 --config RelWithDebInfo `
  --prefix .build\windows-vs2022\game-package --component GameRuntime
```

### 产物位置

| 内容 | 路径 |
| --- | --- |
| 构建出的工具（CLI、测试程序、预览程序） | `.build\windows-vs2022\bin\RelWithDebInfo` |
| 可直接部署的干净运行包 | `.build\windows-vs2022\game-package` |

运行包结构见[安装与更新](../user-guide/installation.md)。CLI、测试程序与界面演示程序保留在 `bin`，不会进入游戏运行包。

## 测试

`build.cmd` / `ctest` 会运行 CTest 注册的分层测试，覆盖 ABI、生命周期、ServiceGraph、Dispatcher、日志、存储、渲染、Pattern、Memory、Pipe、Proxy、HookManager、Plugin Runtime、Profile、Symbol / Validator 与 UE5 / NTE Adapter，以及 Repository 签名 / 事务、网络线程隔离与全局插件启用状态。

> [!NOTE]
> 测试数量以当前构建目录的 `ctest -N` 输出为准，不在文档固定历史数字。

几类关键集成门禁：

- **SDK 集成门** — 先安装 SDK，再从独立 Fixture 仓库执行 configure / build / pack / validate / TestHost。
- **Core 集成** — 真实加载 Core 与代理，调用 `DwmIsCompositionEnabled`，验证服务诊断、模块查询、内存区域、实时扫描与 UE 状态的管道往返，并在退出前停止并等待全部 Runtime worker。
- **D3D12 Fixture** — 连续绘制 1000 帧、执行 50 次 Resize 和 10 次窗口 / SwapChain 重建，验证像素非空并启用 Debug Layer 资源检查。

### 预览界面（无游戏）

直接运行 `anomaly-platform-preview.exe` 可以在独立进程通过与游戏内相同的 Platform 与 PluginManager 路径预览和调试界面与插件。

### AddressSanitizer

ASan 使用独立预设：

```powershell
cmake --preset windows-asan
cmake --build --preset windows-asan --parallel
ctest --preset windows-asan
```

## 发布打包

正式组件包由同一构建树生成并逐包审计：

```powershell
pwsh -NoProfile -File .\tools\package_release.ps1 `
  -BuildDirectory .build\windows-vs2022 `
  -Version 1.0.0 `
  -OutputDirectory .build\release\1.0.0
```

输出确定性组装的四个 ZIP 与校验 / 审计文件：

| 组件 ZIP | 内容 |
| --- | --- |
| **Runtime** | 代理、Core、配置、bundled Profile、五个内建插件 |
| **SDK** | 头文件、CMake 包、四个示例（源码 + 独立 CMake 工程） |
| **Tools** | 六个正式命令行工具 |
| **Symbols** | 与全部发布 PE 双向核对的 PDB |

以及 `SHA256SUMS.txt`、`release-manifest.json`、四份 `audit/*.json` 与四份 SPDX 2.3 `sbom/*.spdx.json`。
这些文件和 Symbols ZIP 用于 CI 内部的完整性与发布策略检查；tag workflow 仅发布 Runtime、SDK、
Tools 三个 ZIP，并为这三个归档生成 GitHub build provenance attestation。

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

`AnomalyLauncher.exe` 与 `AnomalyCrashCoordinator.exe` 随 Runtime 分发；`anomaly-render-fixture.exe` 与 `anomaly-platform-preview.exe` 是构建目录中的测试 / 开发程序。

## 相关

- [架构概览](architecture.md)
- [贡献指南](contributing.md)
- [插件开发](plugin-development.md)
