# 开发者文档

本文档集面向**贡献者与插件作者**。如果你只是想使用 Anomaly，请看[用户文档](../user-guide/README.md)。

## 阅读顺序

1. [从源码构建](building.md) — 环境要求、唯一受支持的构建路径、验证与发布打包。
2. [架构概览](architecture.md) — 设计原则、组件模型、线程域、所有权与 ABI 边界。
3. [插件开发](plugin-development.md) — 用 SDK 从零写一个插件：Manifest、capability、生命周期、热重载、打包。
4. [发布第三方插件](plugin-distribution.md) — 制作下载 ZIP、配置在线插件源 JSON、发布和验证。
5. [贡献指南](contributing.md) — 工作流、提交规范、不可突破的代码边界与验证门禁。

写插件时的接口细节见 [API 参考](../api-reference/README.md)。

## 权威来源

文档描述稳定的接口与流程。以下易变事实以仓库内的源为准：

| 主题 | 权威来源 |
| --- | --- |
| 模块边界与依赖方向 | [架构概览](architecture.md) · `.agents/architecture.md` |
| 协作约定、交付流程、代码边界 | [`AGENTS.md`](../../AGENTS.md) |
| 构建配置 | [`CMakePresets.json`](../../CMakePresets.json) |
| 命令与权限规则 | `.agents/tooling.md` |
| 公开 ABI | `include/anomaly/sdk/` |
| Schema | `schemas/` |

## 快速导航

| 你要做… | 首先阅读 |
| --- | --- |
| 构建并部署 | [从源码构建](building.md) |
| 理解模块边界 | [架构概览](architecture.md) |
| 写一个插件 | [插件开发](plugin-development.md)、[插件模板](https://github.com/AnomalyNTE/Anomaly-Plugin-Template)、[`examples/`](../../examples/README.md) |
| 把插件放进“可用”页面 | [发布第三方插件](plugin-distribution.md) |
| 查接口签名 | [API 参考](../api-reference/README.md)、`include/anomaly/sdk/` |
| 提交改动 | [贡献指南](contributing.md)、[`AGENTS.md`](../../AGENTS.md) |
