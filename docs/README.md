# Anomaly 文档

Anomaly 的文档分为三个文档集，按你的角色选择入口。

## 📘 [用户文档](user-guide/README.md)

面向**使用者**：把 Anomaly 部署到游戏、配置平台、使用内建插件、通过 CLI 做诊断。

- [安装与部署](user-guide/installation.md)
- [快速上手](user-guide/quickstart.md)
- [配置参考](user-guide/configuration.md)
- [内建插件](user-guide/built-in-plugins.md)
- [诊断 CLI](user-guide/diagnostics-cli.md)
- [NTE Build Profile](user-guide/nte-profiles.md)
- [故障排查与 FAQ](user-guide/troubleshooting.md)

## 🛠️ [开发者文档](developer-guide/README.md)

面向**贡献者与插件作者**：从源码构建、理解架构、开发插件、参与贡献。

- [从源码构建](developer-guide/building.md)
- [架构概览](developer-guide/architecture.md)
- [插件开发](developer-guide/plugin-development.md)
- [贡献指南](developer-guide/contributing.md)

## 📑 [API 参考](api-reference/README.md)

面向**插件作者**：完整的纯 C ABI v1 契约。

- [约定与总览](api-reference/README.md)
- [生命周期与 Core 服务](api-reference/lifecycle-and-core.md)
- [平台作用域服务](api-reference/platform-services.md)
- [Interop 与内存](api-reference/interop-and-memory.md)
- [插件间 IPC](api-reference/ipc.md)
- [UI 服务](api-reference/ui-services.md)
- [UE5 服务](api-reference/ue5-services.md)
- [NTE 服务](api-reference/nte-services.md)
- [Manifest 与 capability](api-reference/manifest-and-capabilities.md)

---

> 测试数量、发布清单这类会经常变化的信息，以当前代码和构建输出为准。公开 ABI 和数据格式则以 `include/anomaly/sdk/` 和 `schemas/` 为准。
