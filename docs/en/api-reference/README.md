# API reference

> 🌐 English · [简体中文](../../api-reference/README.md)

This is the complete reference for the Anomaly plugin **pure C ABI v1**. The authoritative definition is always the headers under `include/anomaly/sdk/`; this reference mirrors them and adds semantic notes.

For a workflow-oriented walkthrough, see [Plugin development](../developer-guide/plugin-development.md).

## Pages

| Page | Contents |
| --- | --- |
| [Lifecycle and Core services](lifecycle-and-core.md) | Plugin entry, Host API, descriptors, `anomaly.core`, `plugin-state`, `localization` |
| [Platform scoped services](platform-services.md) | `config`, `storage`, `runtime-info`, `diagnostics`, `scheduler`, `commands`, `notifications` |
| [Interop and memory](interop-and-memory.md) | `interop.signature`, `interop.hook`, `interop.patch`, memory grants |
| [Inter-plugin IPC](ipc.md) | `anomaly.ipc` |
| [WebSocket broadcast](websocket.md) | `anomaly.websocket` |
| [UI services](ui-services.md) | `anomaly.ui`, `window`, `font`, `texture`, `input` |
| [UE5 services](ue5-services.md) | `ue5.build`, `ahud`, `framework`, `names`, `objects`, `world` |
| [NTE services](nte-services.md) | `nte.build`, `session`, `player`, `player-teleport`, `map-landmarks`, `navigation`, `pickup`, `ui-buttons`, `combat`, `skills`, `skill-invocation`, `entities`, `actors`, `metrics` |
| [Manifest and capabilities](manifest-and-capabilities.md) | Manifest v2 schema, capability mapping, status codes |

## ABI conventions

- The headers are pure C (`extern "C"`) and compile from both C and C++. C++ may additionally include `<anomaly/sdk/cpp.hpp>`.
- The calling convention macro `ANOMALY_CALL` is `__cdecl` on Win32.
- The plugin export macro `ANOMALY_SDK_EXPORT` (`__declspec(dllexport)`).
- Never pass STL types, exceptions, RTTI objects, raw UE objects or `ImGuiContext*` across the DLL boundary.

### Version constants (`version.h`)

| Constant | Value |
| --- | --- |
| `ANOMALY_SDK_VERSION_STRING` | The configured release version (official tag builds match the tag, for example `"1.0.1"`) |
| `ANOMALY_PLUGIN_API_V1_MAJOR` | `1` |
| `ANOMALY_PLUGIN_API_V1_MINOR` | `0` |
| `ANOMALY_PLUGIN_V1_ENTRY_NAME` | `"AnomalyPluginEntryV1"` |

## Base types (`base.h`)

```c
typedef struct AnomalyStringViewV1 { const char* data; size_t size; } AnomalyStringViewV1;
typedef struct AnomalyByteSpanV1 { const uint8_t* data; size_t size; } AnomalyByteSpanV1;
typedef struct AnomalyMutableByteSpanV1 { uint8_t* data; size_t size; } AnomalyMutableByteSpanV1;
typedef struct AnomalyGenerationHandleV1 { uint64_t id; uint64_t generation; } AnomalyGenerationHandleV1;
```

### Status codes `AnomalyStatusCodeV1`

```c
typedef struct AnomalyStatusV1 { uint32_t code; uint32_t reserved; AnomalyStringViewV1 message; } AnomalyStatusV1;
```

| Value | Name | Meaning |
| --- | --- | --- |
| 0 | `ANOMALY_STATUS_V1_OK` | Success |
| 1 | `ANOMALY_STATUS_V1_INVALID_ARGUMENT` | Invalid argument |
| 2 | `ANOMALY_STATUS_V1_UNAVAILABLE` | The service / Feature is not published yet or has degraded |
| 3 | `ANOMALY_STATUS_V1_NOT_FOUND` | The target does not exist (for example no saved document, stale cursor) |
| 4 | `ANOMALY_STATUS_V1_BUFFER_TOO_SMALL` | Buffer too small (used with the two-phase buffer protocol) |
| 5 | `ANOMALY_STATUS_V1_FAILED` | Generic failure |
| 6 | `ANOMALY_STATUS_V1_TIMEOUT` | Timeout |
| 7 | `ANOMALY_STATUS_V1_PERMISSION_DENIED` | Not authorized (missing capability / grant) |
| 8 | `ANOMALY_STATUS_V1_CONFLICT` | Conflict |
| 9 | `ANOMALY_STATUS_V1_CANCELLED` | Cancelled |

C++ helpers: `anomaly::sdk::Ok()`, `Succeeded(status)`.

### Feature state `AnomalyFeatureStateV1`

`ANOMALY_FEATURE_V1_UNAVAILABLE = 0`, `ANOMALY_FEATURE_V1_AVAILABLE = 1`. Used for `feature_state(feature_id)` queries.

### Allocator `AnomalyAllocatorV1`

The host exposes `allocate` / `reallocate` / `release` through `AnomalyHostApiV1::allocator`. **Memory allocated by the host is released by the host allocator.**

## Service query model

The host `AnomalyHostApiV1::query_service(host_context, service_id, minimum_version, &table)` returns an opaque service table pointer. Every service table begins with the same three-field prefix:

```c
uint32_t struct_size;      // actual size of this table
uint32_t service_version;  // version of this table
void*    user;             // passed back as the first argument to every function call
```

**Before using a table, a consumer must** validate that `struct_size` covers the fields it is going to call, and confirm that `service_version == 1`. The C++ wrapper `anomaly::sdk::Host::Query<Service>(id, 1)` encapsulates these checks (including the `api_major` validation) and returns an empty `ServiceRef<Service>` on failure. The current ABI provides no legacy service tables or version aliases.

> [!IMPORTANT]
> A service table belongs to **one Host lifecycle generation**. A cached table from a stopped or replaced generation can only return `UNAVAILABLE` (scalar queries return 0), and it does not recover on a later Start generation. Query again after every lifecycle.

## Two-phase buffer protocol

Many functions that return variable-length data use a signature of the form `(..., char* destination, size_t* inout_size)` or `MutableByteSpan + size_t*`:

1. Call first with `destination = NULL` and `*inout_size = 0` → returns the required byte count (written to `*inout_size`), and the status is usually `OK` or `BUFFER_TOO_SMALL`.
2. Call again after allocating a buffer → the data is written and `*inout_size` is updated to the actual written length.

## Generation handle

`AnomalyGenerationHandleV1 { id, generation }` identifies a host resource or object snapshot. Every handle created by Platform / Interop / IPC is bound to a plugin generation; a stale handle is rejected instead of resolving to a later object identity. For resource lifetimes see [Architecture overview · Ownership and lifecycle](../developer-guide/architecture.md#ownership-and-lifecycle).

## Service index

| Service ID | Current version | Required capability | Reference |
| --- | --- | --- | --- |
| `anomaly.core` | 1 | (memory operations also require `memory-read` / `memory-write`) | [Core](lifecycle-and-core.md#anomalycore) |
| `anomaly.plugin-state` | 1 | `configuration` | [plugin-state](lifecycle-and-core.md#anomalyplugin-state) |
| `anomaly.localization` | 1 | `ui` | [localization](lifecycle-and-core.md#anomalylocalization) |
| `anomaly.config` | 1 | `configuration` | [config](platform-services.md#anomalyconfig) |
| `anomaly.storage` | 1 | `storage` | [storage](platform-services.md#anomalystorage) |
| `anomaly.json` | 1 | `json` | [json](platform-services.md#anomalyjson) |
| `anomaly.runtime-info` | 1 | `runtime-info` | [runtime-info](platform-services.md#anomalyruntime-info) |
| `anomaly.diagnostics` | 1 | `diagnostics` | [diagnostics](platform-services.md#anomalydiagnostics) |
| `anomaly.scheduler` | 1 | `scheduler` | [scheduler](platform-services.md#anomalyscheduler) |
| `anomaly.commands` | 1 | `commands` | [commands](platform-services.md#anomalycommands) |
| `anomaly.notifications` | 1 | `notifications` | [notifications](platform-services.md#anomalynotifications) |
| `anomaly.interop.signature` | 1 | `interop-signature` | [signature](interop-and-memory.md#anomalyinteropsignature) |
| `anomaly.interop.hook` | 1 | `interop-hook` | [hook](interop-and-memory.md#anomalyinterophook) |
| `anomaly.interop.patch` | 1 | `interop-patch` | [patch](interop-and-memory.md#anomalyinteroppatch) |
| `anomaly.ipc` | 1 | `ipc` | [ipc](ipc.md) |
| `anomaly.websocket` | 1 | `websocket` | [websocket](websocket.md) |
| `anomaly.ui` | 1 | `ui` | [ui](ui-services.md#anomalyui) |
| `anomaly.window` | 1 | `ui-window` | [window](ui-services.md#anomalywindow) |
| `anomaly.font` | 1 | `ui-font` | [font](ui-services.md#anomalyfont) |
| `anomaly.texture` | 1 | `ui-texture` | [texture](ui-services.md#anomalytexture) |
| `anomaly.input` | 1 | `input` | [input](ui-services.md#anomalyinput) |
| `anomaly.ue5.build` | 1 | `ue5-build` | [ue5.build](ue5-services.md#anomalyue5build) |
| `anomaly.ue5.ahud` | 1 | `ue5-ahud` | [ue5.ahud](ue5-services.md#anomalyue5ahud) |
| `anomaly.ue5.framework` | 1 | `game-events` | [ue5.framework](ue5-services.md#anomalyue5framework) |
| `anomaly.ue5.names` | 1 | `ue5-names` | [ue5.names](ue5-services.md#anomalyue5names) |
| `anomaly.ue5.objects` | 1 | `ue5-objects` | [ue5.objects](ue5-services.md#anomalyue5objects) |
| `anomaly.ue5.world` | 1 | `ue5-world` | [ue5.world](ue5-services.md#anomalyue5world) |
| `anomaly.ue5.streaming-source` | 1 | `ue5-streaming-source` | [ue5.streaming-source](ue5-services.md#anomalyue5streaming-source) |
| `anomaly.nte.build` | 1 | `nte-build` | [nte.build](nte-services.md#anomalyntebuild) |
| `anomaly.nte.esc-menu-button` | 1 | `nte-esc-menu-button` | [nte.esc-menu-button](nte-services.md#anomalynteesc-menu-button) |
| `anomaly.nte.session` | 1 | `nte-session-snapshot` | [nte.session](nte-services.md#anomalyntesession) |
| `anomaly.nte.player` | 1 | `nte-player-snapshot` | [nte.player](nte-services.md#anomalynteplayer) |
| `anomaly.nte.player-teleport` | 1 | `nte-player-teleport` | [nte.player-teleport](nte-services.md#anomalynteplayer-teleport) |
| `anomaly.nte.player-hold` | 1 | `nte-player-hold` | [nte.player-hold](nte-services.md#anomalynteplayer-hold) |
| `anomaly.nte.map-landmarks` | 1 | `nte-map-landmarks` | [nte.map-landmarks](nte-services.md#anomalyntemap-landmarks) |
| `anomaly.nte.navigation` | 1 | `nte-navigation` | [nte.navigation](nte-services.md#anomalyntenavigation) |
| `anomaly.nte.pickup` | 1 | `nte-pickup` | [nte.pickup](nte-services.md#anomalyntepickup) |
| `anomaly.nte.ui-buttons` | 1 | `nte-ui-buttons` | [nte.ui-buttons](nte-services.md#anomalynteui-buttons) |
| `anomaly.nte.entities` | 1 | `nte-entity-snapshot` | [nte.entities](nte-services.md#anomalynteentities) |
| `anomaly.nte.actors` | 1 | `nte-actor-snapshot` | [nte.actors](nte-services.md#anomalynteactors) |
| `anomaly.nte.combat` | 1 | `nte-combat-read` | [nte.combat](nte-services.md#anomalyntecombat) |
| `anomaly.nte.skills` | 1 | `nte-skills-read` | [nte.skills](nte-services.md#anomalynteskills) |
| `anomaly.nte.skill-invocation` | 1 | `nte-skill-invocation` | [nte.skill-invocation](nte-services.md#anomalynteskill-invocation) |
| `anomaly.nte.metrics` | 1 | `nte-snapshot-metrics` | [nte.metrics](nte-services.md#anomalyntemetrics) |

All public services use version 1. An optional service may still be unavailable because of capabilities, the active Profile or a Feature gate, and plugins must degrade per service.
