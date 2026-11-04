# Lifecycle and Core services

Related headers: `plugin.h`, `services/core.h`, `services/plugin_state.h`, `services/localization.h`. For common conventions see [API reference overview](README.md).

## Plugin entry

A plugin DLL exports a function named `AnomalyPluginEntryV1` (= `ANOMALY_PLUGIN_V1_ENTRY_NAME`); the host calls it to fill in the descriptor:

```c
typedef AnomalyStatusV1 (ANOMALY_CALL *AnomalyPluginEntryV1Fn)(
    AnomalyPluginDescriptorV1* descriptor);
```

The entry should first verify that `descriptor->struct_size` covers the base fields and `on_draw`, then
populate those fields. Optional V1 tail fields may be written only when the host-provided `struct_size`
covers their position, preserving compatibility with older hosts.

### `AnomalyPluginDescriptorV1`

```c
typedef struct AnomalyPluginDescriptorV1 {
    uint32_t struct_size; uint16_t api_major; uint16_t api_minor;
    AnomalyStringViewV1 id; AnomalyStringViewV1 name;
    AnomalyStringViewV1 author; AnomalyStringViewV1 version;
    AnomalyStatusV1 (ANOMALY_CALL *on_load)(const AnomalyHostApiV1* host, void** plugin_context);
    AnomalyStatusV1 (ANOMALY_CALL *on_start)(void* plugin_context);
    AnomalyStatusV1 (ANOMALY_CALL *on_stop)(void* plugin_context, uint32_t deadline_milliseconds);
    void (ANOMALY_CALL *on_unload)(void* plugin_context);
    void (ANOMALY_CALL *on_update)(void* plugin_context, double delta_seconds);
    void (ANOMALY_CALL *on_draw)(void* plugin_context, const AnomalyUiServiceV1* ui);
    void (ANOMALY_CALL *on_overlay)(void* plugin_context, const AnomalyUiServiceV1* ui);
} AnomalyPluginDescriptorV1;
```

| Field | Description |
| --- | --- |
| `api_major` / `api_minor` | Set to `ANOMALY_PLUGIN_API_V1_MAJOR` / `_MINOR` |
| `id` / `name` / `author` / `version` | Plugin metadata (must match the manifest) |
| `on_load` | Query services and register resources; return your own context pointer through `plugin_context` |
| `on_start` | Activate |
| `on_stop` | Stop and commit state within `deadline_milliseconds` |
| `on_unload` | Clean up before unload |
| `on_update` | Per-tick logic update (Game domain); may be `NULL` |
| `on_draw` | Per-frame drawing (Render domain); may be `NULL` |
| `on_overlay` | Runs every render frame in the Render domain even while the plugin window is closed; use it only for persistent world overlays such as `draw_screen_primitives`; may be `NULL` |

On stop or hot reload, the host first freezes ordinary callback sources and waits for callbacks that
are already running. It then calls `on_stop`; the plugin can still release its hooks and other scoped
resources there. The host revokes any leftovers as a fallback. If that fallback cannot drain safely,
the plugin generation is quarantined and its DLL is not unmapped immediately.

`on_overlay` is an optional tail field in the V1 descriptor. The host calls it only when `struct_size`
covers the field and the callback is non-null. Plugins that need to run against older hosts can keep a
fallback drawing path in `on_draw`.

For callback thread domains see [Plugin development · lifecycle](../developer-guide/plugin-development.md#3-lifecycle-and-entry-point).

### `AnomalyHostApiV1`

`on_load` receives the host API:

```c
typedef struct AnomalyHostApiV1 {
    uint32_t struct_size; uint16_t api_major; uint16_t api_minor; void* host_context;
    AnomalyAllocatorV1 allocator;
    AnomalyStatusV1 (ANOMALY_CALL *query_service)(void* host_context,
        AnomalyStringViewV1 service_id, uint32_t minimum_version, const void** service);
} AnomalyHostApiV1;
```

`query_service` is the only entry point for obtaining any service table. The C++ wrapper `anomaly::sdk::Host(host).Query<T>(id, version)` is safer.

---

## `anomaly.core`

- **ID**: `ANOMALY_CORE_SERVICE_V1_ID` = `"anomaly.core"`
- **Version**: `ANOMALY_CORE_SERVICE_V1_VERSION` = 1
- **capability**: none required (but `read_memory` / `write_memory` require the `memory-read` / `memory-write` grant respectively)

```c
typedef struct AnomalyCoreServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    void (ANOMALY_CALL *log)(void* user, uint32_t level, AnomalyStringViewV1 message);
    AnomalyStatusV1 (ANOMALY_CALL *read_memory)(void* user, uintptr_t address, AnomalyMutableByteSpanV1 destination);
    AnomalyStatusV1 (ANOMALY_CALL *write_memory)(void* user, uintptr_t address, AnomalyByteSpanV1 source);
    AnomalyStatusV1 (ANOMALY_CALL *plugin_directory)(void* user, char* destination, size_t* inout_size);
} AnomalyCoreServiceV1;
```

| Function | Description |
| --- | --- |
| `log(level, message)` | Write a log entry; `level` is described below |
| `read_memory(address, destination)` | Read process memory into `destination`; requires `memory-read`, otherwise `PERMISSION_DENIED` |
| `write_memory(address, source)` | Write process memory; requires `memory-write`, otherwise `PERMISSION_DENIED` |
| `plugin_directory(destination, inout_size)` | Get this plugin's package directory (two-phase buffer protocol) |

Log levels `AnomalyCoreLogLevelV1`: `TRACE=0`, `INFO=1`, `WARNING=2`, `ERROR=3`.

> [!NOTE]
> `anomaly.core` does not grant raw memory access automatically; new plugins should prefer the semantic [NTE snapshot service](nte-services.md) over reading game objects themselves.

---

## `anomaly.plugin-state`

- **ID**: `"anomaly.plugin-state"` · **Version** 1 · **capability** `configuration`

```c
typedef struct AnomalyPluginStateServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *directory)(void* user, char* destination, size_t* inout_size);
} AnomalyPluginStateServiceV1;
```

`directory` returns the plugin's private state directory (two-phase buffer protocol). For persisted structured settings, prefer [`anomaly.config`](platform-services.md#anomalyconfig).

---

## `anomaly.localization`

- **ID**: `"anomaly.localization"` · **Version** 1 · **capability** `ui`

```c
typedef struct AnomalyLocalizationServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *locale)(void* user, char* destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *translate)(void* user,
        AnomalyStringViewV1 key, AnomalyStringViewV1 english_fallback,
        const AnomalyStringViewV1* arguments, size_t argument_count,
        char* destination, size_t* inout_size);
} AnomalyLocalizationServiceV1;
```

| Function | Description |
| --- | --- |
| `locale(destination, inout_size)` | Current locale (for example `zh-CN`), two-phase buffer protocol |
| `translate(key, english_fallback, arguments, argument_count, destination, inout_size)` | Translate by `key`, falling back to `english_fallback` when the key is missing; `arguments` is an array of positional arguments (at most 8). With `destination = NULL` it follows the two-phase buffer protocol and only reports the required byte count (including the trailing NUL), with status `OK` or `BUFFER_TOO_SMALL` |

> [!NOTE]
> `anomaly.localization` and `anomaly.ui` share the `ui` capability.

### Message catalog

A plugin supplies its translations in `locales/<locale>.json` inside its own package directory (next to `manifest.json`; the file name is the locale name):

```json
{
  "schemaVersion": 1,
  "locale": "zh-CN",
  "messages": {
    "window.title": "我的插件",
    "plugin.description": "展示当前玩家与会话诊断。",
    "status.cached": "已缓存 {0} 条"
  }
}
```

| Constraint | Description |
| --- | --- |
| `schemaVersion` | Must be `1` |
| `locale` | Must equal the requested locale name |
| key | Only `[a-z0-9._-]`, at most 255 bytes |
| message | A non-empty string |
| placeholders | Only `{0}`..`{7}`; escape literal braces as `{{` / `}}`. **A localized message must declare exactly the placeholders of its English fallback**, otherwise that entry silently falls back to English |
| size | File at most 1 MiB, at most 4096 entries |

**The catalog is all-or-nothing**: one invalid entry (illegal key, empty message, unescaped brace, malformed placeholder) invalidates the whole file, so every `translate` of that plugin serves English and a single `plugin localization fallback` line is logged. Validate the file as a whole after adding entries instead of expecting per-entry tolerance.

The host also reads two fixed keys from the same file for the management UI: `window.title` as the display name and `plugin.description` as the blurb, falling back to the manifest `name` / `description` only when they are missing. Keep those two manifest fields English and put the Chinese in the catalog.

The active language is fixed for the lifetime of the process and changing it requires a game restart, so text can be resolved wherever it is produced instead of at draw time.

On the C++ side use the SDK's `anomaly::sdk::Localizer` (`include/anomaly/sdk/localization.hpp`); it wraps `Text` / `Format` / `Label` and the two-phase retry for long messages. See [Plugin development · Localized text](../developer-guide/plugin-development.md#localized-text).
