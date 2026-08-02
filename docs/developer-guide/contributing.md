# 贡献指南

欢迎贡献。本页概括工作流与不可突破的边界。**协作约定的权威来源是仓库根目录的 [`AGENTS.md`](../../AGENTS.md)**；提交改动前请通读它。

## 交付流程

1. 开始前执行 `git status --short --branch`，保留与当前任务无关的已修改和未跟踪文件。
2. 编辑前确定功能所属**模块与公开边界**。以最小改动完成需求，不夹带无关重构或格式化。
3. 行为变化必须在**所属层**补充或更新验证；先运行最窄的相关验证，再按影响扩大范围。
4. 完成前执行 `git diff --check`，检查暂存差异，并说明实际执行的验证或未执行原因。
5. 每一项源码 / 构建 / CI / Profile / Schema / 运行时行为 / 工具脚本变更，都要有对应的**原子 Git commit**。纯文档改动也应提交，除非明确要求不提交。

## 提交规范

采用 **Conventional Commit** 风格：

```text
fix(plugin): drain callbacks before unload
build(cmake): unify the local preset entry point
docs(sdk): document the config schema migration flow
```

- 只暂存明确文件，**禁止 `git add -A`**。
- 不改写他人的提交，不用 `reset --hard` 或强制 checkout 隐藏工作。
- 一个提交只能包含当前任务的文件。无法创建预期提交时，必须明确说明阻塞原因。

## 不可突破的代码边界

以下规则来自 [`AGENTS.md`](../../AGENTS.md)，是架构完整性的底线：

- `src/bootstrap/dwmapi/` 必须在 **Loader Lock 外**工作：仅转发系统导出并异步启动 Core，不放入配置、Hook、UI、扫描或插件加载。
- `src/runtime/` 负责 Session 状态、停止传播、服务生命周期与 Dispatcher 所有权。部分启动失败时，已启动服务也必须按依赖**反序**停止。
- `src/platform/`、`src/services/`、`src/diagnostics/` 保持**游戏无关**，不依赖 NTE 布局。
- `src/game/ue5/` 负责已验证的通用 UE 符号；`src/game/nte/` 负责按来源优先级选择活动 Profile 与 Feature Gate。新偏移 / 签名必须有 **Profile、validator 与降级行为**，不能硬编码在插件或 UI 中。
- `src/plugin/` 负责 Catalog、包、generation、Scope、热重载与故障隔离。每个插件可见资源都要记录在 `PluginScope`，回调不越出 generation lease。
- `src/render/dx12/` 与 `src/ui/` 的 Render 回调**不得**扫描包、加载 DLL、等待生命周期任务或做同步 I/O。
- `include/anomaly/sdk/` 是公开 DLL 边界：仅纯 C ABI、定宽类型、opaque handle、清晰所有权与 version / `struct_size` 检查。不得暴露 STL、异常、RTTI、裸 UE 对象、`ImGuiContext*` 或内部头文件。

完整的模块、线程与变更影响图见[架构概览](architecture.md)与 `.agents/architecture.md`。

## 构建与验证

主仓库统一使用 `windows-vs2022` 配置预设与 `windows-relwithdebinfo` 构建预设（AddressSanitizer 用 `windows-asan`）。`build.cmd` 只是 CI 同一命令序列的便捷包装。**禁止**新建临时 NMake / Ninja / `build/` 或按阶段命名的主构建树。命令见[从源码构建](building.md)。

## 实时目标与数据安全

> [!CAUTION]
> 实时进程工具只用于明确的诊断任务，不能作为日常验证。`anomaly-cli` 的 `write` / `patch` / `protect` / `alloc` / `free` 会修改其他进程，必须得到明确授权，且目标**只能是自有测试进程**。Profile 重扫和实体扫描即使以读取为主，也需要明确请求与活动 Profile 上下文。

日志、dump、Profile 数据与诊断包都可能含敏感信息，分享前必须审查并脱敏。

## 文档

- 用户可见的行为变化应同步更新[用户文档](../user-guide/README.md)。
- 公开接口变化应同步更新 [API 参考](../api-reference/README.md)。
- 文档描述稳定接口与流程；易变数字（fingerprint、发布清单）留在代码与构建输出中，不写进文档以免漂移。
