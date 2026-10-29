# Manifest and capabilities

Schema: [`schemas/plugin-manifest.schema.json`](../../../schemas/plugin-manifest.schema.json); capability policy source: `src/plugin/plugin_capability_policy.cpp`. For the workflow, see [Plugin development](../developer-guide/plugin-development.md).

## Manifest v2

Every plugin directory package must contain `manifest.json` (`schemaVersion` is always `2`).

### Fields

| Field | Required | Description |
| --- | --- | --- |
| `schemaVersion` | ✅ | Constant `2` |
| `id` | ✅ | Normalized ID: dot-separated lowercase labels (for example `anomaly.example.my-plugin`), 3–255 characters |
| `name` | ✅ | Display name, 1–128 characters |
| `description` | No | Plugin summary, 1–512 characters; the host shows it in the "About" section of the plugin details |
| `version` | ✅ | SemVer string |
| `entry` | ✅ | Entry DLL file name; must end with `.dll` |
| `api` | ✅ | `{ "major", "minMinor", "maxMinor" }`, declares the supported plugin API version range |
| `games` | ✅ | Array of target game ids (1–16 entries, for example `["nte"]`) |
| `builds` | ✅ | Build compatibility declaration (for example `["nte-*"]` or an exact fingerprint); each pattern belongs to a declared game |
| `loadPhase` | ✅ | Currently only `"game-ready"` is allowed |
| `author` | — | Author name |
| `license` | — | A single SPDX license identifier (**required for public distribution**) |
| `audience` | — | `"user"` or `"developer"` |
| `dependencies` | — | Array of plugin dependencies, see below |
| `services` | — | Array of required services, see below |
| `capabilities` | — | Array of declared capabilities (unique, ≤64 entries) |

### Dependencies and services

```jsonc
// dependencies[] element
{ "id": "<qualifiedId>", "version": "<range>", "optional": false }
// services[] element
{ "id": "<qualifiedId>", "minVersion": 1, "optional": false }
```

### Example

```json
{
  "schemaVersion": 2,
  "id": "anomaly.example.my-plugin",
  "name": "My Plugin",
  "description": "Shows current player and session diagnostics.",
  "author": "Me",
  "license": "MIT",
  "version": "1.0.0",
  "entry": "plugin.dll",
  "api": {"major": 1, "minMinor": 0, "maxMinor": 0},
  "games": ["nte"],
  "builds": ["nte-*"],
  "loadPhase": "game-ready",
  "services": [
    {"id": "anomaly.ui", "minVersion": 1},
    {"id": "anomaly.nte.entities", "minVersion": 1, "optional": true}
  ],
  "capabilities": ["ui", "nte-entity-snapshot"]
}
```

## Loading rules

- A plugin must be a **directory package that contains a manifest**; a root-level DLL and a directory without a manifest are not loaded.
- During loading the whole package is copied into an immutable shadow generation; after the file handle is opened, the entry and resource paths are validated once more and must still resolve inside the package root.
- Before activation, static and supported delay-load imports, private CRT and system DLLs, and already-loaded modules with the same name are pre-checked; dependency conflicts that resist isolation are rejected.

`DiscoverPluginCatalog` validates `games`, `builds`, the API range and service versions only when the caller passes a `PluginCompatibilityContext`. The current production Runtime does not pass that context when it discovers local plugins, so `builds` is parsed but never used to block loading. Plugins must rely on service availability, signature checks and layout validation for actual degradation.

## Capability enforcement

Capabilities constrain **service visibility and resource ownership** (they are not a native code sandbox). Validation happens before the DLL is mapped. Any of the following makes the plugin's authorization **unenforceable / rejected**:

- The plugin declares an **unknown capability**.
- A required service that is **not optional** and is not `anomaly.core` has no corresponding capability mapping.
- A required non-optional service has a mapping, but the plugin **does not declare** the corresponding capability.
- `schemaVersion` is not the current value (`2`).

If the plugin queries a service it is not authorized for at runtime, the call returns `ANOMALY_STATUS_V1_PERMISSION_DENIED` (distinct from `UNAVAILABLE`, which means the service is not published yet).

> [!NOTE]
> A service declared with `optional: true` does not take part in the enforcement mapping checks above, and its unavailability does not block plugin loading — this is how graceful degradation works.

## Capability mapping

Every required (non-optional) service needs the capability listed in the table below:

| Service ID | Required capability |
| --- | --- |
| `anomaly.core` | (none; for `read_memory` / `write_memory` see the memory grants below) |
| `anomaly.plugin-state` | `configuration` |
| `anomaly.config` | `configuration` |
| `anomaly.storage` | `storage` |
| `anomaly.json` | `json` |
| `anomaly.runtime-info` | `runtime-info` |
| `anomaly.diagnostics` | `diagnostics` |
| `anomaly.scheduler` | `scheduler` |
| `anomaly.ipc` | `ipc` |
| `anomaly.websocket` | `websocket` |
| `anomaly.commands` | `commands` |
| `anomaly.notifications` | `notifications` |
| `anomaly.interop.signature` | `interop-signature` |
| `anomaly.interop.hook` | `interop-hook` |
| `anomaly.interop.patch` | `interop-patch` |
| `anomaly.ui` | `ui` |
| `anomaly.localization` | `ui` |
| `anomaly.window` | `ui-window` |
| `anomaly.font` | `ui-font` |
| `anomaly.texture` | `ui-texture` |
| `anomaly.input` | `input` |
| `anomaly.ue5.build` | `ue5-build` |
| `anomaly.ue5.ahud` | `ue5-ahud` |
| `anomaly.ue5.framework` | `game-events` |
| `anomaly.ue5.names` | `ue5-names` |
| `anomaly.ue5.objects` | `ue5-objects` |
| `anomaly.ue5.world` | `ue5-world` |
| `anomaly.ue5.streaming-source` | `ue5-streaming-source` |
| `anomaly.nte.build` | `nte-build` |
| `anomaly.nte.esc-menu-button` | `nte-esc-menu-button` |
| `anomaly.nte.session` | `nte-session-snapshot` |
| `anomaly.nte.metrics` | `nte-snapshot-metrics` |
| `anomaly.nte.player` | `nte-player-snapshot` |
| `anomaly.nte.player-teleport` | `nte-player-teleport` |
| `anomaly.nte.player-hold` | `nte-player-hold` |
| `anomaly.nte.map-landmarks` | `nte-map-landmarks` |
| `anomaly.nte.navigation` | `nte-navigation` |
| `anomaly.nte.pickup` | `nte-pickup` |
| `anomaly.nte.ui-buttons` | `nte-ui-buttons` |
| `anomaly.nte.entities` | `nte-entity-snapshot` |
| `anomaly.nte.actors` | `nte-actor-snapshot` |
| `anomaly.nte.combat` | `nte-combat-read` |
| `anomaly.nte.skills` | `nte-skills-read` |
| `anomaly.nte.skill-invocation` | `nte-skill-invocation` |

### Standalone grants (not bound to service queries)

| capability | Grants |
| --- | --- |
| `memory-read` | `read_memory` on `anomaly.core` |
| `memory-write` | `write_memory` on `anomaly.core` |
| `entity-esp` | ABI v1 compatible `anomaly.ui` ESP draw calls (`draw_entity_bbox` / `draw_entity_box3d` / `draw_entity_label`); new plugins should declare `ue5-ahud` and subscribe to `anomaly.ue5.ahud` |

These three capabilities are known capabilities, but they are not derived automatically from the service mapping — a plugin declares them explicitly in `capabilities` when it needs them.

## Status codes

Plugin calls return `AnomalyStatusV1`; for the `code` values see [API reference overview · Status codes](README.md#status-codes-anomalystatuscodev1). Two common codes related to authorization:

- `ANOMALY_STATUS_V1_PERMISSION_DENIED` — the required capability / grant is not declared.
- `ANOMALY_STATUS_V1_UNAVAILABLE` — the service / Feature is not published yet or has degraded.
