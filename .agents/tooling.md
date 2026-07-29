# 构建、测试与工具规则

## 主仓库标准构建

前置条件：Windows x64、Visual Studio 2022 C++ Build Tools、Windows SDK、CMake 3.22+，以及
PowerShell 7+（`pwsh`，供测试和脚本使用）。共享构建树固定为 `.build/windows-vs2022`，禁止新建
竞争性的主构建树。`build.cmd` 是下面序列的便捷包装，预期产物与 CI 完全一致：

```powershell
cmake --preset windows-vs2022
cmake --build --preset windows-relwithdebinfo --parallel
ctest --preset windows-relwithdebinfo
cmake --install .build\windows-vs2022 --config RelWithDebInfo `
  --prefix .build\windows-vs2022\game-package --component GameRuntime
```

AddressSanitizer 使用：`cmake --preset windows-asan`、
`cmake --build --preset windows-asan --parallel`、`ctest --preset windows-asan`。
日常二进制位于 `.build/windows-vs2022/bin/RelWithDebInfo`，可部署 Runtime 位于
`.build/windows-vs2022/game-package`。

## 验证范围

验证应沿改动的 target、模块归属和依赖关系收敛：先构建直接受影响的 target，再运行对应的单元、
契约或集成测试。跨共享模块边界、构建图、公开 ABI 或发布打包的改动需要扩大到受影响的依赖方和
合同，但不因目录或模块名称自动升级为完整测试。只有影响无法可靠界定或用户明确要求时，才运行
完整 `ctest --preset windows-relwithdebinfo`。

| 改动类型 | 最低验证 |
| --- | --- |
| 仅 Markdown | `git diff --check`，并检查链接和路径 |
| 单个实现单元或模块内行为 | 构建直接受影响的 target，并以 `ctest --preset windows-relwithdebinfo -R <name>` 运行对应测试 |
| 跨共享模块边界或公共合同 | 构建受影响模块及直接依赖方，运行两侧相关单测、契约与集成测试 |
| CMake 或构建图 | 重新 configure，构建受影响 target，并运行受影响测试；仅在影响无法界定时扩大到完整门禁 |
| 公开 SDK/ABI | 构建 SDK 直接依赖方，运行 ABI、外部消费者及相关集成测试 |
| 内存安全、解析器、重载、所有权或停止 | 完成上述相关验证，并增加对应的定向 `windows-asan` 测试 |
| 发布包 | 构建发布所需 target、运行相关合同/集成测试，再使用标准构建树运行 `tools/package_release.ps1` |

环境限制导致命令无法执行时，运行可执行的最强子集，并在完成说明中记录未执行命令及原因。
不能因为某个构建目录存在，就宣称发布包或真实 NTE Smoke 已验证。

## 项目工具

| 工具 | 用途 | 边界 |
| --- | --- | --- |
| `anomaly-profile` | 离线诊断 fingerprint 和 Profile 校验 | fingerprint 只用于诊断/证据/cache，不作为 Profile 激活条件 |
| `anomaly-plugin` | 校验或打包本地插件包 | 只处理受控目录；真实热重载前必须先校验 |
| `anomaly-test-host` | 无游戏进程的插件生命周期测试 | 真实游戏进程之前的首选验证方式 |
| `anomaly-cli` 只读命令 | `status`、`modules`、`sections`、`regions`、`scan`、`ue`、`read`、`chain`、`snapshot` | 目标只能是自有进程；snapshot 和日志可能含敏感路径/数据 |
| `anomaly-cli` 修改命令 | `write`、`patch`、`protect`、`alloc`、`free` | 需要用户明确授权和自有测试目标；不得作为常规诊断手段 |
| `diagnose_nte_profile.ps1`、`rescan_profile_signatures.ps1`、`scan_nte_entities.ps1` | 检查实时 NTE 目标和活动 Profile | 必须有明确请求、正确 PID/Profile；生成数据分享前需审查 |
| `start-coordinate-tracker.ps1` | 启动常驻后台 tracker | 仅在明确请求时使用；结束前必须运行 `stop-coordinate-tracker.ps1` |
| `collect_diagnostics.ps1` | 生成脱敏诊断包 | 用 `pwsh -NoProfile -File` 启动；检查输入 Runtime 和输出路径，dump 必须人工审查 |
| `package_release.ps1` | staging、审计并覆盖发布输出 | 用 `pwsh -NoProfile -File` 启动；仅在干净标准构建后，且指定明确版本/输出目录时运行 |
| `run_stability_suite.ps1` | 显式 opt-in 的 soak 证据 | 用 `pwsh -NoProfile -File` 启动；`-Quick` 只用于脚本冒烟，不是发布证据 |

所有工具脚本默认使用 `.build/windows-vs2022` 和其中的 `RelWithDebInfo` 二进制。只在有意使用
其他 artifact 时传入明确的工具路径；不要重新引入阶段命名或临时构建目录默认值。

## Git 操作

日常循环使用 `git status --short --branch`、`git diff --check`、`git diff --staged` 与
`git add <明确路径>`。每项实现改动创建一个原子、Conventional Commit 风格的提交。禁止暂存
`.build/`、`data/`、`runtime/`、日志、dump 或其他任务的未跟踪文件。
