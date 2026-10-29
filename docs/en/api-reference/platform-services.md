# Platform-scoped services

Corresponding header: `services/platform.h`. These services are independently versioned scoped service tables, and each one requires its own capability. For general conventions see [API reference overview](README.md).

## Common types

```c
typedef enum AnomalyNotificationSeverityV1 {
    ANOMALY_NOTIFICATION_V1_INFO = 0, ANOMALY_NOTIFICATION_V1_WARNING = 1,
    ANOMALY_NOTIFICATION_V1_ERROR = 2
} AnomalyNotificationSeverityV1;

typedef struct AnomalyRuntimeInfoV1 {
    uint32_t struct_size;
    uint32_t runtime_version_major, runtime_version_minor, runtime_version_patch;
    uint32_t process_id, thread_id;
    uint64_t uptime_milliseconds, plugin_generation;
} AnomalyRuntimeInfoV1;
```

Callback typedefs:

```c
typedef AnomalyStatusV1 (ANOMALY_CALL *AnomalyConfigMigrationV1)(void* user,
    uint32_t source_schema_version, AnomalyByteSpanV1 source,
    AnomalyMutableByteSpanV1 destination, size_t* inout_size);
typedef AnomalyStatusV1 (ANOMALY_CALL *AnomalyDiagnosticSelfTestV1)(void* user,
    AnomalyMutableByteSpanV1 destination, size_t* inout_size);
typedef void (ANOMALY_CALL *AnomalyTaskCallbackV1)(void* user, AnomalyGenerationHandleV1 task);
typedef AnomalyStatusV1 (ANOMALY_CALL *AnomalyCommandCallbackV1)(void* user,
    AnomalyStringViewV1 arguments, AnomalyMutableByteSpanV1 destination, size_t* inout_size);
```

---

## `anomaly.config`

- **ID**: `"anomaly.config"` · **Version** 1 · **capability** `configuration`

Manages the **plugin-local** JSON document: register a schema, then read / write atomically / migrate. The host owns the persistence location (`Anomaly\config\plugins\`).

```c
typedef struct AnomalyConfigServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *register_schema)(void* user,
        AnomalyStringViewV1 schema_id, uint32_t schema_version,
        AnomalyByteSpanV1 schema_json, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *unregister_schema)(void* user, AnomalyGenerationHandleV1 handle);
    AnomalyStatusV1 (ANOMALY_CALL *read)(void* user, AnomalyStringViewV1 schema_id,
        uint32_t* schema_version, AnomalyMutableByteSpanV1 destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *write_atomic)(void* user, AnomalyStringViewV1 schema_id,
        uint32_t schema_version, AnomalyByteSpanV1 document);
    AnomalyStatusV1 (ANOMALY_CALL *migrate)(void* user, AnomalyStringViewV1 schema_id,
        AnomalyConfigMigrationV1 migration, void* migration_user);
} AnomalyConfigServiceV1;
```

| Function | Description |
| --- | --- |
| `register_schema` | Registers a stable `schema_id` in **every loaded generation**; only after that can you read / write / migrate. The returned handle is owned by the Scope |
| `unregister_schema` | Unregisters the schema handle |
| `read` | Reads the persisted document through the two-phase buffer protocol; `NOT_FOUND` means nothing has been saved yet; `schema_version` returns the stored version |
| `write_atomic` | **Atomically replaces** the document after validating it against the registered schema |
| `migrate` | Uses a callback to migrate an older document version to the current schema |

For usage see [`examples/reliable_config`](../../../examples/reliable_config/plugin.cpp).

---

## `anomaly.storage`

- **ID**: `"anomaly.storage"` · **Version** 1 · **capability** `storage`

Plugin-private raw file read / atomic write / delete (relative paths).

```c
typedef struct AnomalyStorageServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *read)(void* user, AnomalyStringViewV1 relative_path,
        AnomalyMutableByteSpanV1 destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *write_atomic)(void* user,
        AnomalyStringViewV1 relative_path, AnomalyByteSpanV1 source);
    AnomalyStatusV1 (ANOMALY_CALL *remove)(void* user, AnomalyStringViewV1 relative_path);
} AnomalyStorageServiceV1;
```

Prefer `anomaly.config` for structured settings; `storage` suits binary or non-schema data.

---

## `anomaly.json`

- **ID**: `"anomaly.json"` · **Version** 1 · **capability** `json`

Provides plugins with scoped, lightweight JSON DOM parsing / traversal / serialization. Every returned handle is bound to a plugin generation and is invalidated by the Scope when the plugin stops; a plugin may also actively `release` a handle it no longer needs.

```c
typedef enum AnomalyJsonKindV1 {
    ANOMALY_JSON_V1_NULL = 0, ANOMALY_JSON_V1_BOOLEAN = 1,
    ANOMALY_JSON_V1_NUMBER = 2, ANOMALY_JSON_V1_STRING = 3,
    ANOMALY_JSON_V1_ARRAY = 4, ANOMALY_JSON_V1_OBJECT = 5
} AnomalyJsonKindV1;

typedef struct AnomalyJsonServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *parse)(void* user, AnomalyStringViewV1 document,
        AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *release)(void* user, AnomalyGenerationHandleV1 handle);
    AnomalyStatusV1 (ANOMALY_CALL *kind)(void* user, AnomalyGenerationHandleV1 handle,
        uint32_t* kind);
    AnomalyStatusV1 (ANOMALY_CALL *boolean_value)(void* user,
        AnomalyGenerationHandleV1 handle, int32_t* value);
    AnomalyStatusV1 (ANOMALY_CALL *number_value)(void* user,
        AnomalyGenerationHandleV1 handle, double* value);
    AnomalyStatusV1 (ANOMALY_CALL *string_value)(void* user,
        AnomalyGenerationHandleV1 handle, char* destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *array_size)(void* user,
        AnomalyGenerationHandleV1 handle, size_t* size);
    AnomalyStatusV1 (ANOMALY_CALL *array_item)(void* user,
        AnomalyGenerationHandleV1 handle, size_t index, AnomalyGenerationHandleV1* child);
    AnomalyStatusV1 (ANOMALY_CALL *object_size)(void* user,
        AnomalyGenerationHandleV1 handle, size_t* size);
    AnomalyStatusV1 (ANOMALY_CALL *object_key_at)(void* user,
        AnomalyGenerationHandleV1 handle, size_t index, char* destination,
        size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *object_find)(void* user,
        AnomalyGenerationHandleV1 handle, AnomalyStringViewV1 key,
        AnomalyGenerationHandleV1* child);
    AnomalyStatusV1 (ANOMALY_CALL *serialize)(void* user,
        AnomalyGenerationHandleV1 handle, char* destination, size_t* inout_size);
} AnomalyJsonServiceV1;
```

`parse` accepts a complete UTF-8 JSON document; `string_value` / `serialize` / `object_key_at` follow the two-phase buffer protocol. The child handles returned by `array_item` and `object_find` must also be released, or they are invalidated along with the plugin generation.

---

## `anomaly.runtime-info`

- **ID**: `"anomaly.runtime-info"` · **Version** 1 · **capability** `runtime-info`

```c
typedef struct AnomalyRuntimeInfoServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *snapshot)(void* user, AnomalyRuntimeInfoV1* snapshot);
    AnomalyStatusV1 (ANOMALY_CALL *runtime_version_utf8)(void* user, char* destination, size_t* inout_size);
} AnomalyRuntimeInfoServiceV1;
```

`snapshot` fills an `AnomalyRuntimeInfoV1` (version, PID, TID, uptime, plugin generation); `runtime_version_utf8` returns the version string (two-phase buffer).

---

## `anomaly.diagnostics`

- **ID**: `"anomaly.diagnostics"` · **Version** 1 · **capability** `diagnostics`

Register self tests, run self tests, export a diagnostic snapshot.

```c
typedef struct AnomalyDiagnosticsServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *register_self_test)(void* user, AnomalyStringViewV1 id,
        AnomalyDiagnosticSelfTestV1 callback, void* callback_user, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *unregister_self_test)(void* user, AnomalyGenerationHandleV1 handle);
    AnomalyStatusV1 (ANOMALY_CALL *run_self_test)(void* user, AnomalyStringViewV1 id,
        AnomalyMutableByteSpanV1 destination, size_t* inout_size);
    AnomalyStatusV1 (ANOMALY_CALL *snapshot_json)(void* user,
        AnomalyMutableByteSpanV1 destination, size_t* inout_size);
} AnomalyDiagnosticsServiceV1;
```

> [!NOTE]
> This is a different layer from the [diagnostics named pipe protocol](../user-guide/diagnostics-cli.md): here a plugin registers self tests with the host, whereas the pipe protocol lets an external client send commands to the Runtime.

---

## `anomaly.scheduler`

- **ID**: `"anomaly.scheduler"` · **Version** 1 · **capability** `scheduler`

```c
typedef struct AnomalySchedulerServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *schedule)(void* user, uint32_t delay_milliseconds,
        AnomalyTaskCallbackV1 callback, void* callback_user, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *cancel)(void* user, AnomalyGenerationHandleV1 handle);
} AnomalySchedulerServiceV1;
```

`schedule` invokes the callback after `delay_milliseconds`; the returned handle is bound to the generation, tasks that have not started are cancelled automatically on stop / reload, and you can also cancel one actively with `cancel`.

---

## `anomaly.commands`

- **ID**: `"anomaly.commands"` · **Version** 1 · **capability** `commands`

```c
typedef struct AnomalyCommandsServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *register_command)(void* user, AnomalyStringViewV1 name,
        AnomalyStringViewV1 description, AnomalyCommandCallbackV1 callback,
        void* callback_user, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *unregister_command)(void* user, AnomalyGenerationHandleV1 handle);
    AnomalyStatusV1 (ANOMALY_CALL *invoke)(void* user, AnomalyStringViewV1 name,
        AnomalyStringViewV1 arguments, AnomalyMutableByteSpanV1 destination, size_t* inout_size);
} AnomalyCommandsServiceV1;
```

Register named commands; the command callback receives `arguments` and writes to `destination` (two-phase buffer); `invoke` calls a registered command.

---

## `anomaly.notifications`

- **ID**: `"anomaly.notifications"` · **Version** 1 · **capability** `notifications`

```c
typedef struct AnomalyNotificationsServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *post)(void* user, AnomalyNotificationSeverityV1 severity,
        AnomalyStringViewV1 title, AnomalyStringViewV1 body,
        uint32_t timeout_milliseconds, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *dismiss)(void* user, AnomalyGenerationHandleV1 handle);
} AnomalyNotificationsServiceV1;
```

`post` publishes a notification (severity + title + body + timeout); the returned handle can be closed actively with `dismiss`.
