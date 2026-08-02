# Agent 辅助资料

`.agents/` 是由根目录 `AGENTS.md` 显式引用的项目资料目录，本身不是 Codex 的自动加载入口。
不要在这里放置密钥、本机路径、生成证据或个人偏好，也不要把它作为 Runtime 配置或发布包内容。

建议阅读顺序：

1. `../AGENTS.md`：强制交付流程、Git、风险操作和边界规则。
2. `architecture.md`：源码所有权、依赖方向和线程域。
3. `tooling.md`：构建、验证、打包、诊断与实时工具的使用约束。
4. `../ARCHITECTURE.md`、`../README.md` 与相关 ADR：请求涉及的详细实现事实。

这里只记录稳定、项目专属、能直接指导实现决策的规则。产品行为写入 README，架构理由写入 ADR，
发布证据写入 `docs/release/` 或 `docs/baseline/`。
