# Anomaly Agent 协作约定

本文件是面向所有 AI 编码 Agent 的协作约定，遵循通用的 `AGENTS.md` 规范。多数 Agent
（Codex、Claude Code、Cursor、Gemini CLI 等）会自动读取层级中的 `AGENTS.md`；不自动加载的
Agent 请在会话开始时手动引用本文件。它是仓库唯一的自动加载入口，`.agents/` 仅存放由本文件
显式引用的项目资料，不依赖目录自动发现。

在修改源码、构建、Profile、部署或发布资料前，先阅读本文件。它补充产品文档，不能覆盖
更高优先级的平台指令或用户请求。

## 信息来源

- `README.md`：项目概览、已支持能力与快速上手。
- `docs/user-guide/`：面向使用者的安装、配置、内建插件与诊断文档。
- `docs/developer-guide/`：从源码构建、架构概览、插件开发与贡献流程。
- `docs/api-reference/`：完整的纯 C ABI v1 契约。
- `.agents/architecture.md`：实现时使用的模块归属、依赖和线程边界速查。
- `.agents/tooling.md`：构建、测试、发布和实时进程工具的命令与权限规则。
- `CMakePresets.json`：主仓库唯一受支持的构建配置。

`.build/`、`data/`、`runtime/` 都是本机或生成状态，不能作为源码输入。不要修改生成的
CMake 文件、构建产物或正在运行的游戏文件来掩盖源码问题。

## 必经交付流程

1. 开始前执行 `git status --short --branch`，保留与当前任务无关的已修改和未跟踪文件。
2. 编辑前确定功能所属模块和公开边界。以最小改动完成需求，不夹带无关重构或格式化。
3. 行为变化必须在所属层补充或更新验证。验证范围以实际改动和依赖关系为准：构建直接受影响的
   target，运行对应的单元、契约或集成验证；仅在跨共享模块边界、修改构建图/公开 ABI/发布打包，
   或用户明确要求时扩大范围。不得仅因改动属于 Runtime、Launcher、CMake 或工具等类别就默认扩大
   验证范围。具体命令见 `.agents/tooling.md`。
4. 完成前执行 `git diff --check`，检查暂存差异，并说明实际执行的验证或未执行原因。
5. `.local/` 仅用于本机临时 fixture；其中的源码、测试代码和构建脚本禁止暂存、提交或通过
   `git add -f` 强制加入。需要纳入版本控制的实现必须移到受版本管理的模块目录。

提交不是每项修改的必需步骤。只有用户或任务流程明确要求提交时才创建 Git commit；需要提交时，
使用 Conventional Commit 风格，例如 `fix(plugin): drain callbacks before unload`、
`build(cmake): unify the local preset entry point`。只暂存明确文件，禁止 `git add -A`；不要改写
他人的提交，也不要用 `reset --hard` 或强制 checkout 隐藏工作。

除非用户明确要求，不生成额外的审计记录、补丁副本或回滚脚本；验证结果直接在任务回复中说明。
禁止生成回滚副本：不要创建 `.bak`、带时间戳的 DLL/manifest 副本或其他回滚文件；热更新直接覆盖运行包，验证结果直接在任务回复中说明。

## 实现与测试纪律

- 禁止过度防御代码。只为已知合同、明确故障模型或可复现问题增加校验；同一事实在同一同步调用中
  不得反复验证，不得为纯假设竞态堆叠状态机、重试、缓存或分支。新增防御分支时，必须能说明它
  防止的具体故障、所属边界以及为何现有边界不能处理。
- 禁止构建自圆其说的测试。测试必须从公开合同或独立观察到的行为出发，并能在实现错误时失败；
  不得复制实现算法、为测试专门开放生产路径，或用与实现相同的假设同时构造夹具和预期结果。
  优先验证用户可见结果、公开 ABI 和真实解析输入，避免只证明 mock 按预设返回 mock 值。
- 实现复杂度应与当前需求成比例。未经需求或证据支持，不提前实现兼容层、未来版本、恢复策略或
  极端场景框架；发现这些代码时应先删除或收敛，再继续叠加功能。

## 架构边界

`RuntimeSession` 是进程级生命周期所有者，`src/runtime/core_main.cpp` 是生产组合根。
依赖从 Bootstrap 指向 Runtime 抽象，再经由声明接口到平台服务和外部能力。Runtime 内核不能
包含 NTE 专用头文件，插件不能包含宿主内部头文件，跨线程域工作必须通过 Dispatcher，不能直接
触碰其他组件的所有者。

以下规则不可突破：

- `src/bootstrap/dwmapi/` 必须在 Loader Lock 外工作：仅转发系统导出并异步启动 Core，不放入
  配置、Hook、UI、扫描或插件加载。
- `src/runtime/` 负责 Session 状态、停止传播、服务生命周期和 Dispatcher 所有权。部分启动失败时，
  已启动服务也必须按依赖反序停止。
- `src/platform/windows/`、`src/services/`、`src/diagnostics/` 保持游戏无关，不能依赖 NTE 布局
  或发布游戏专用策略。
- `src/game/ue5/` 负责经过验证的通用 UE 符号和快照；`src/game/nte/` 负责按来源优先级选择活动
  Profile 和 NTE Feature Gate。Profile 不包含或匹配 Build identity；新增偏移或签名必须有 Profile、validator 与降级行为，不能硬编码在插件
  或 UI 中。
- `src/plugin/` 负责 Catalog、包、generation、Scope、热重载和故障隔离。每个插件可见资源都要
  记录在 `PluginScope`，回调不能超出其 generation lease。
- `src/render/dx12/` 与 `src/ui/` 负责 Present/Resize/输入/UI 生命周期。Render 回调不得扫描
  包、加载 DLL、等待生命周期任务或同步执行文件/网络 I/O。
- `include/anomaly/sdk/` 是公开 DLL 边界：仅纯 C ABI、定宽类型、opaque handle、清晰所有权以及
  version/`struct_size` 检查。不得暴露 STL、异常、RTTI 对象、裸 UE 对象、`ImGuiContext*` 或内部头文件。
- Core 以手工映射方式运行，映像不能携带 loader-managed 静态 TLS。链接进 `anomaly_core` 的代码禁止
  `thread_local`，也禁止会拉入 `_Init_thread_epoch` 的函数级动态初始化局部静态；线程本地状态一律改用
  `include/anomaly/thread_local_value.hpp` 的 FLS 封装，新链接进 Core 的 target 必须登记到
  `CMakeLists.txt` 的 `ANOMALY_MANUAL_MAP_TLS_FREE_TARGETS`，并确认构建出的核心映像没有 `.tls` 段、
  TLS 目录为空。

完整的模块、线程和变更影响图见 `.agents/architecture.md`。

## 插件文案与本地化（强制）

每个插件都必须走 i18n。新增插件、修改插件文案、审查插件 PR 时按下面执行；接口与目录格式以
`docs/api-reference/lifecycle-and-core.md#anomalylocalization` 为准。

- **每个插件都要有本地化路径**：带任何用户可见文本的插件，包目录里必须有 `locales/zh-CN.json`
  （`schemaVersion: 1`、`locale: "zh-CN"`、`messages`），并在 `manifest.json` 的 `services` 声明
  `{"id": "anomaly.localization", "minVersion": 1}`（capability 仍是共用的 `ui`）。没有 UI 的纯诊断
  插件可以不带目录，但同样禁止任何中文字符串字面量。
- **不许硬编码文案**：所有用户可见文本经 `anomaly.localization` 取，C++ 用 SDK 的
  `anomaly::sdk::Localizer`（`include/anomaly/sdk/localization.hpp`）；C++ 字面量一律写成英文回退，
  中文只存在于 `locales/<locale>.json`。中文注释可以保留，受约束的只有字符串字面量。
- **manifest 交英文回退**：`name` / `description` 写英文，中文显示名放目录的 `window.title`、中文简介
  放 `plugin.description`——宿主用这两个 key 渲染管理界面，缺失时才回退到 manifest。
- **单一 Localizer 来源**：在 `Load(host, …)` 里从 host 查一次并保存，不得新增第二个实例，也不得在
  静态初始化阶段调用本地化服务；语言在进程内固定，文案可以在产生它的位置解析。
- **占位符必须与英文回退逐字对齐**：只允许 `{0}`..`{7}`，字面花括号写 `{{` / `}}`；集合不一致时宿主
  静默回退英文，不报错——所以 `+` 拼接数值一律改成 `Format` 加 `{N}`。
- **目录整体生效**：任何一条不合规（key 字符集/长度、空消息、花括号未转义）都会让整个目录失效、该插件
  全部回退英文。改完必须整体校验 key 覆盖、占位符掩码、残留中文字面量、manifest 回退语言——统一用
  `pwsh -NoProfile -File tools/check_plugin_i18n.ps1 -Plugin <名字>`（省略 `-Plugin` 则检查全部插件），
  它必须全绿。
- **游戏数据不是文案**：DataTable 行名、游戏对话框原文、外部格式的原始标识（如 MMD 骨骼/表情名）必须与
  运行时数据逐字匹配，不参与本地化；保持原样或写成 `\uXXXX` 转义，并就地注明它是数据。判断标准是
  「改变它会不会破坏匹配」。
- **打包时机**：`anomaly_add_plugin` 在 configure 期决定是否镜像 `locales/`。新增本地化目录后必须重新
  configure 再构建，否则包目录里不会出现 `zh-CN.json`；manifest 与目录是 POST_BUILD 拷贝，改完要重建对应
  target 才会更新构建树里的包。

## 构建与测试约定

主仓库统一使用 `windows-vs2022` 配置预设与 `windows-relwithdebinfo` 构建预设。
`build.cmd` 只是 CI 同一命令序列的便捷包装。禁止新建临时 NMake、Ninja、`build/` 或按阶段命名的
主构建树。默认构建只产出 Runtime、Tools、SDK 三个发布组件；开发夹具与 PDB 不属于
发布内容，需要 `build.cmd fixtures|symbols` 显式开启。精确命令与产物路径见
`.agents/tooling.md`。

## 冗余清理约定

清理冗余时：
删除必须有调用图或链接级证据，不能仅凭“看起来没用”移除公开合同、schema getter、Retry API、
旧 ABI table accessor 或测试专用 parser；集中安全原语后，reparse、原子性、超时、rollback 与
quarantine 测试不得弱化；ABI `struct_size`/version 检查、PluginScope generation/lease/ledger、
ABI/线程/回调最外层 `catch (...)`、Dispatcher 锁外销毁 callback 等属于有意设计，不得为缩减行数删除。

## 实时目标与数据安全

实时进程工具只用于明确的诊断任务，不能作为日常验证。`anomaly-cli write`、`patch`、`protect`、
`alloc`、`free` 会修改其他进程，必须得到用户明确授权，并且目标只能是自有测试进程。Profile
重扫和实体扫描即使以读取为主，也需要明确请求和活动 Profile 上下文。后台 tracker 必须在
结束前使用配套 stop 脚本停止。日志、dump、Profile 数据和诊断包都可能含敏感信息，分享前必须审查
并脱敏。

## 快速导航

| 目标 | 首先阅读 |
| --- | --- |
| Runtime 启动与生命周期 | `src/bootstrap/dwmapi/proxy_main.cpp`、`src/runtime/core_main.cpp`、`src/runtime/runtime_session.cpp` |
| 服务与所有权 | `src/runtime/service_graph.cpp`、`src/runtime/runtime_dispatchers.cpp` |
| 插件 ABI 与热重载 | `include/anomaly/sdk/`、`src/plugin/`、`examples/` |
| 插件文案与本地化 | `include/anomaly/sdk/localization.hpp`、`plugins/*/locales/`、`tools/check_plugin_i18n.ps1` |
| UE5/NTE 兼容 | `src/game/ue5/`、`src/game/nte/`、`profiles/nte/` |
| 渲染与 UI | `src/render/dx12/`、`src/ui/` |
| 验证与发布门禁 | `CMakeLists.txt`、`.agents/tooling.md` |
