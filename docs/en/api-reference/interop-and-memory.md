# Interop and memory

Corresponding headers: `services/interop.h` (signatures / hooks / patches) and `services/core.h` (raw memory). For general conventions, see [API reference overview](README.md).

> [!CAUTION]
> The services on this page modify process execution state directly. Every handle is bound to a plugin generation: a tracked patch restores the original bytes on release / stop / reload, and a function hook's detour must be wrapped in `begin_callback` / `end_callback`.

## Raw memory grant

The `read_memory` / `write_memory` of `anomaly.core` are not granted automatically along with the service; they are separate grants:

| Call | Required capability | When missing |
| --- | --- | --- |
| `read_memory` | `memory-read` | `PERMISSION_DENIED` |
| `write_memory` | `memory-write` | `PERMISSION_DENIED` |

See [`anomaly.core`](lifecycle-and-core.md#anomalycore) for the signature. New plugins should prefer the semantic [NTE services](nte-services.md).

---

## `anomaly.interop.signature`

- **ID**: `"anomaly.interop.signature"` · **Version** 1 / 2 · **capability** `interop-signature`

```c
typedef struct AnomalySignatureServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *resolve)(void* user,
        AnomalyStringViewV1 module_name, AnomalyStringViewV1 section_name,
        AnomalyStringViewV1 pattern, uintptr_t* address);
} AnomalySignatureServiceV1;
```

Querying version 1 always returns the original `AnomalySignatureServiceV1`. `resolve` matches a
byte signature within the given module / section (supporting `??`, `4?`, `?F` wildcards) and writes
`*address` on a unique hit; multiple matches return `CONFLICT`. Lifecycle / Worker calls learn
instructions and attempt recovery when the original signature has zero matches. v1 preserves object
members and stack displacements and does not update field constants inside the plugin. Game / Render
calls perform no learning, recovery, or state file I/O.

### Version 2: resolve addresses and field layouts together

Querying version 2 returns a separate `AnomalySignatureServiceV2`, which keeps the original `resolve` and adds:

```c
AnomalyStatusV1 (ANOMALY_CALL *resolve_layout)(
    void* user, const AnomalySignatureLayoutRequestV2* request,
    uintptr_t* addresses, uint32_t address_count,
    uint32_t* offsets, uint32_t offset_count);
```

Declarations are owned by the plugin and do not need to migrate into the overall Profile. Every struct must fill in `struct_size`; see `anomaly/sdk/services/interop.h` for the full definitions:

| Declaration | Contents |
| --- | --- |
| `AnomalySignatureFieldV2` | unique field `id`, original `seed_offset`, `alignment` (1 / 2 / 4 / 8) |
| `AnomalySignatureMemberV2` | `field_index`, `addend`, access `width` (1 / 2 / 4 / 8 bytes), allowed `access` flags |
| `AnomalySignatureWitnessV2` | unique function `id`, original `pattern`, member binding array and count |
| `AnomalySignatureLayoutRequestV2` | module, executable section, field array and count, function array and count |

`access` combines `ANOMALY_SIGNATURE_MEMBER_READ_V2`, `ANOMALY_SIGNATURE_MEMBER_WRITE_V2`, and
`ANOMALY_SIGNATURE_MEMBER_ADDRESS_V2`.
Address computation does not access the field, so its access width is not checked; read and write
operations must match the declared width.
The extraction relation is "object member displacement in the instruction = field offset + addend";
for example, use `addend = 8` when learning an array base through `TArray::Num`. Seed offsets only
mark the semantic location in the old instruction; a failed resolve never falls back to them.

Each group holds at most 32 fields and 32 functions, each function at most 32 bindings, and pattern
text at most 1024 bytes. A function signature starts at a Windows x64 function entry, with the entry
RCX as the object. The framework tracks object provenance through supported MOV / LEA instructions,
unlocks only the bound member displacements, and preserves other members, stack locations, plain
immediates, and instruction structure. The scan window is at most 128 bytes, with at least 4
complete instructions and 16 fixed bytes; for functions with fewer than 24 fixed bytes, every field
must have another distinct function address that produces a consistent result. All extraction points
and function evidence for a field must agree and satisfy `alignment`; ambiguity or conflicting
evidence returns `CONFLICT`, and insufficient evidence or zero matches returns `NOT_FOUND`.

The call runs synchronously and is allowed only from Lifecycle / Worker; Game / Render returns `UNAVAILABLE`.
The caller allocates the output arrays, whose counts must equal `witness_count` and `field_count` respectively, and results return in declaration order.
Once the plugin context is valid and the request prefix, output pointers, and counts are valid, any failure zeroes both output groups; on success the whole group is published.
No handle or host object is allocated across the DLL boundary. Status messages must be copied before the next layout call on the current thread.

Learning records are stored alongside v1 in `signature-knowledge/signatures.json` under each plugin's
state directory, isolated by module, section, original signature, and field binding; a group is saved
only after it passes validation. Corrupt records are ignored, and a failed save does not invalidate
already validated results. Degradation is still preserved when the function is rewritten, object
provenance is unclear, the access width changes, or the instruction encoding changes. This is
recovery based on the semantic evidence the plugin supplies, not automatic semantic recognition of
arbitrary new fields.

C++ plugins can use the `FieldSeed` / `MemberUse` / `Witness` declarations and `ResolveLayout` from
[`anomaly/sdk/signature.hpp`](../../../include/anomaly/sdk/signature.hpp). The returned snapshot is held
locally by the plugin; `address(id)` / `offset(id)` lookups perform no scanning and no disk access;
querying an undeclared ID throws `std::out_of_range`.

---

## `anomaly.interop.hook`

- **ID**: `"anomaly.interop.hook"` · **Version** 1 · **capability** `interop-hook`

A single plugin generation can create multiple function hooks on different functions.

```c
typedef enum AnomalyHookKindV1 {
    ANOMALY_HOOK_V1_FUNCTION = 1, ANOMALY_HOOK_V1_IAT = 2,
    ANOMALY_HOOK_V1_EXPORT = 3, ANOMALY_HOOK_V1_VTABLE = 4
} AnomalyHookKindV1;

typedef struct AnomalyHookRequestV1 {
    uint32_t struct_size; uint32_t kind;   // AnomalyHookKindV1
    uintptr_t target; void* detour; AnomalyStringViewV1 label;
} AnomalyHookRequestV1;

typedef struct AnomalyHookServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *create)(void* user, const AnomalyHookRequestV1* request,
        uintptr_t* original, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *release)(void* user, AnomalyGenerationHandleV1 handle);
    AnomalyStatusV1 (ANOMALY_CALL *begin_callback)(void* user,
        AnomalyGenerationHandleV1 hook, AnomalyGenerationHandleV1* callback_lease);
    AnomalyStatusV1 (ANOMALY_CALL *end_callback)(void* user, AnomalyGenerationHandleV1 callback_lease);
} AnomalyHookServiceV1;
```

| Function | Description |
| --- | --- |
| `create(request, original, handle)` | installs the hook; `original` returns the entry point for calling the original function |
| `release(handle)` | uninstalls the hook |
| `begin_callback(hook, callback_lease)` | acquires a callback lease when entering the detour |
| `end_callback(callback_lease)` | returns the lease when leaving the detour |

> [!IMPORTANT]
> A detour implementation **must** be wrapped in a paired `begin_callback` / `end_callback`; the host relies on this to drain safely on stop / unload. The current implementation's hook kind is mainly `ANOMALY_HOOK_V1_FUNCTION`. A HookManager that has an owner only starts, stops, and reclaims its own hooks and does not use a global switch.

---

## `anomaly.interop.patch`

- **ID**: `"anomaly.interop.patch"` · **Version** 1 · **capability** `interop-patch`

```c
typedef struct AnomalyPatchServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *apply)(void* user, uintptr_t address,
        AnomalyByteSpanV1 replacement, AnomalyStringViewV1 label, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *release)(void* user, AnomalyGenerationHandleV1 handle);
} AnomalyPatchServiceV1;
```

| Function | Description |
| --- | --- |
| `apply(address, replacement, label, handle)` | writes the `replacement` bytes and saves the original bytes; returns a tracked handle |
| `release(handle)` | restores the original bytes |

> [!NOTE]
> **Recoverable patches must use `anomaly.interop.patch`** (rather than calling `write_memory` directly), so the host can deterministically restore the original bytes on release / stop / reload.
