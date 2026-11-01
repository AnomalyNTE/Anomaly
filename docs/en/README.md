# Anomaly documentation

> 🌐 English · [简体中文](../README.md)

Anomaly's documentation is split into three documentation sets. Pick the entry point that matches your role.

## 📘 [User documentation](user-guide/README.md)

For **users**: deploy Anomaly into the game, configure the platform, use the built-in plugins, and run diagnostics through the CLI.

- [Installation and deployment](user-guide/installation.md)
- [Quick start](user-guide/quickstart.md)
- [Configuration reference](user-guide/configuration.md)
- [Built-in plugins](user-guide/built-in-plugins.md)
- [Diagnostics CLI](user-guide/diagnostics-cli.md)
- [NTE Build Profile](user-guide/nte-profiles.md)
- [Troubleshooting and FAQ](user-guide/troubleshooting.md)

## 🛠️ [Developer documentation](developer-guide/README.md)

For **contributors and plugin authors**: build from source, understand the architecture, develop plugins, and contribute.

- [Building from source](developer-guide/building.md)
- [Architecture overview](developer-guide/architecture.md)
- [Plugin development](developer-guide/plugin-development.md)
- [Contributing guide](developer-guide/contributing.md)

## 📑 [API reference](api-reference/README.md)

For **plugin authors**: the complete pure C ABI v1 contract.

- [Conventions and overview](api-reference/README.md)
- [Lifecycle and Core services](api-reference/lifecycle-and-core.md)
- [Platform-scoped services](api-reference/platform-services.md)
- [Interop and memory](api-reference/interop-and-memory.md)
- [Inter-plugin IPC](api-reference/ipc.md)
- [UI services](api-reference/ui-services.md)
- [UE5 services](api-reference/ue5-services.md)
- [NTE services](api-reference/nte-services.md)
- [Manifest and capabilities](api-reference/manifest-and-capabilities.md)

---

> Information that changes often, such as the release manifest, follows the current code and build output. The public ABI and data formats follow `include/anomaly/sdk/` and `schemas/`.
>
> The Chinese documentation under [`docs/`](../README.md) is the primary source, and this tree mirrors it page by page. Update both languages when you change a page.
