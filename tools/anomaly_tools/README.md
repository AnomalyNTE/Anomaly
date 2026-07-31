# Anomaly Tools DXGI scaffold

本目录是面向开发者的独立 DXGI 代理脚手架，用来加载开发者自行构建的
[`Encryqed/Dumper-7`](https://github.com/Encryqed/Dumper-7)。它不属于 Anomaly Runtime，
不会替换现有 `dwmapi.dll`，也不会由主 CMake、启动器或正式 Release 自动构建、安装或分发。

脚手架只保留已经在当前 HTGame 构建上验证过的 `dxgi.dll` 路径。构建脚本会检查本机
System32 DXGI 的完整导出表，并核对构建产物的名称与 ordinal。脚本只生成 staging 目录，
不会修改游戏文件。

> [!CAUTION]
> Dumper-7 固定版本 `774601d652c868c8e42665ed8f69483faf7c05a4` 没有提供许可证文件。
> 本仓库因此不包含或再分发其源码和二进制。clone、修改、构建和使用前，请自行确认上游
> 许可、游戏条款和适用法律，并只在获准的研究或开发环境中使用。

## 目录内容

| 文件 | 用途 |
| --- | --- |
| `dumper-7-nte.patch` | NTE `GObjects` 布局、签名和 DLL 相邻 INI 路径补丁 |
| `build_dxgi.ps1` | 构建代理、核对导出表，并组装本地 staging 目录 |
| `dxgi_proxy.cpp` / `dxgi_stubs.asm` | 20 个系统 DXGI 导出的透明转发与异步 Dumper 加载 |
| `Dumper-7.ini` | 等待 F8 后开始生成；无自动超时 |

## 前置条件

- Windows x64；
- Visual Studio 2022 C++ Build Tools 和 Windows SDK；
- CMake 3.22+、Git、PowerShell 7+；
- 已退出的 HTGame 进程；
- Anomaly 与 Dumper-7 放在开发者可写的源码目录中。

下面用环境变量表示目录，不依赖特定用户名或绝对路径：

```powershell
$Anomaly = 'C:\src\Anomaly'
$Dumper = 'C:\src\Dumper-7'
```

## 1. Clone 并修改 Dumper-7

补丁以经过验证的上游提交为基线。不要直接对未知的新版本强行应用；上游变更后应重新验证
签名、对象数组布局和配置加载逻辑。

```powershell
git clone https://github.com/Encryqed/Dumper-7.git $Dumper
git -C $Dumper checkout --detach 774601d652c868c8e42665ed8f69483faf7c05a4

$Patch = Join-Path $Anomaly 'tools\anomaly_tools\dumper-7-nte.patch'
git -C $Dumper apply --check --unidiff-zero --ignore-space-change $Patch
git -C $Dumper apply --unidiff-zero --ignore-space-change $Patch
```

补丁做两件事：

1. 使用当前 NTE 的 `GObjects` 运行时签名，解析 RIP-relative 地址，并按 `0x10`、`0x20`、
   `0x24`、`0x28`、`0x2C` 布局和 `0x10000` chunk size 初始化对象数组；
2. 从 `Dumper-7.dll` 所在目录读取 `Dumper-7.ini`，避免官方启动器改变工作目录后跳过 F8
   等待并过早扫描。

需要长期维护这项修改时，应 fork Dumper-7，在 fork 中提交补丁，并让团队从固定 commit
构建；不要依赖未提交的本地工作区。

## 2. 编译 Dumper-7

```powershell
Set-Location $Dumper
cmake --preset vs2022
cmake --build --preset vs2022-Release --parallel
```

预期产物：

```text
out/build/vs2022/bin/Release/Dumper-7.dll
```

## 3. 构建 DXGI 脚手架

```powershell
$DumperDll = Join-Path $Dumper 'out\build\vs2022\bin\Release\Dumper-7.dll'
pwsh -NoProfile -File (Join-Path $Anomaly 'tools\anomaly_tools\build_dxgi.ps1') `
  -DumperPath $DumperDll
```

构建脚本会检查：

- System32 `dxgi.dll` 导出表与脚手架的 20 个导出一致；
- 构建出的代理包含相同的 20 个名称和 ordinal；
- 没有遗漏或额外的 ordinal-only 导出。

产物位于：

```text
.build/windows-vs2022/bin/RelWithDebInfo/anomaly-tools/dxgi/
  dxgi.dll
  AnomalyToolsDXGI.pdb
  Dumper-7.dll
  Dumper-7.ini
```

PDB 只用于本地诊断，不需要复制到游戏目录。

## 4. 接入游戏

1. 退出 HTGame，确认任务管理器中没有残留进程；
2. 检查 `HTGame.exe` 同目录是否已有 `dxgi.dll`，不要覆盖来源不明的同名文件；
3. 从 staging 目录复制 `dxgi.dll`、`Dumper-7.dll`、`Dumper-7.ini` 到 `HTGame.exe` 同目录；
4. 正常启动游戏。Dumper 控制台应显示 `Press F8 to begin dump.`；
5. 等游戏窗口出现并进入游戏后按 F8；
6. 控制台应先显示 `Resolved NTE GObjects at offset 0x...`，再继续生成。

代理会把加载结果追加到同目录的 `anomaly-tools.log`。正常加载记录为：

```text
Anomaly Tools DXGI: Dumper-7.dll loaded.
```

要移除脚手架，先退出游戏，再删除确认属于本工具的 `dxgi.dll`、`Dumper-7.dll`、
`Dumper-7.ini` 和 `anomaly-tools.log`。不要删除 System32 中的系统 DXGI。

## 故障定位

| 现象 | 检查 |
| --- | --- |
| 没有 `anomaly-tools.log` | 游戏是否实际加载了应用目录 `dxgi.dll` |
| 日志显示 Dumper 加载失败 | `Dumper-7.dll` 是否与代理同目录且为 x64 |
| 没显示 F8 提示就开始扫描 | 补丁是否应用，INI 是否与 Dumper DLL 同目录 |
| `GObjects couldn't be found` | 游戏版本可能已变；重新验证签名和 NTE 对象数组布局 |
| 游戏窗口出现前退出 | 查看 Windows WER 的故障模块和偏移，不要用固定延迟掩盖卸载或初始化错误 |

此脚手架不接入 Anomaly 的 `GameRuntime`、`Tools` 或 Release 组件。正式发布物不会包含
Dumper-7；开发者构建出的 staging 目录由开发者自行保管和审查。
