# Developer documentation

> 🌐 English · [简体中文](../../developer-guide/README.md)

This documentation set is for **contributors and plugin authors**. If you only want to use Anomaly, see the [user guide](../user-guide/README.md).

## Reading order

1. [Building from source](building.md) — requirements, the only supported build path, validation, and release packaging.
2. [Architecture overview](architecture.md) — design principles, component model, thread domains, ownership, and ABI boundaries.
3. [Plugin development](plugin-development.md) — write a plugin from scratch with the SDK: manifest, capability, lifecycle, hot reload, packaging.
4. [Publishing third-party plugins](plugin-distribution.md) — produce the download ZIP, configure the online plugin source JSON, publish, and validate.
5. [Contributing guide](contributing.md) — workflow, commit conventions, inviolable code boundaries, and validation gates.

For interface details while writing a plugin, see the [API reference](../api-reference/README.md).

## Authoritative sources

The documentation describes stable interfaces and workflows. The following volatile facts defer to the sources inside the repository:

| Topic | Authoritative source |
| --- | --- |
| Module boundaries and dependency direction | [Architecture overview](architecture.md) · `.agents/architecture.md` |
| Collaboration conventions, delivery workflow, code boundaries | [`AGENTS.md`](../../../AGENTS.md) |
| Build configuration | [`CMakePresets.json`](../../../CMakePresets.json) |
| Command and permission rules | `.agents/tooling.md` |
| Public ABI | `include/anomaly/sdk/` |
| Schema | `schemas/` |

## Quick navigation

| You want to… | Read first |
| --- | --- |
| Build and deploy | [Building from source](building.md) |
| Understand module boundaries | [Architecture overview](architecture.md) |
| Write a plugin | [Plugin development](plugin-development.md), [Plugin template](https://github.com/AnomalyNTE/Anomaly-Plugin-Template), [`examples/`](../../../examples/README.md) |
| Get a plugin onto the "Available" page | [Publishing third-party plugins](plugin-distribution.md) |
| Look up an interface signature | [API reference](../api-reference/README.md), `include/anomaly/sdk/` |
| Submit a change | [Contributing guide](contributing.md), [`AGENTS.md`](../../../AGENTS.md) |
