# Plugin development

This page walks you through writing a plugin from scratch with the Anomaly SDK. **The complete interface signatures live in the [API reference](../api-reference/README.md)**; this page covers the workflow and the key points. The repository contains four ready-to-build examples in [`examples/`](../../../examples/README.md).

To start a standalone plugin repository, begin from the [Anomaly Plugin Template](https://github.com/AnomalyNTE/Anomaly-Plugin-Template): click **Use this template** on GitHub, then follow the template notes to replace the plugin ID, name, author and capability. The template already ships a CMake project, a manifest, localization resources and a runnable UI Gallery example.

## Core concepts

A plugin is a **directory package**:

```text
Anomaly\plugins\MyPlugin\
  manifest.json     declares ID, version, API range, dependencies, required services and capabilities
  plugin.dll        exports AnomalyPluginEntryV1
  assets\           (optional) assets
  dependency.dll    (optional) native dependency
```

- On load the whole package is copied into an **immutable shadow generation**, so assets and native dependencies share the same snapshot as the DLL.
- Root-level DLLs and directories without a manifest **are not loaded**.
- A plugin hooks in by exporting `AnomalyPluginEntryV1` and filling in an `AnomalyPluginDescriptorV1`. The ABI v1 entry only carries the **lifecycle** and **`query_service`**.

## 1. Install the SDK

A plugin repository only needs the installed SDK, not the Runtime sources or an import library:

```powershell
cmake --install C:\path\to\anomaly-build --prefix C:\AnomalySDK --component SDK
```

## 2. Minimal plugin project

```cmake
cmake_minimum_required(VERSION 3.22)
project(MyPlugin LANGUAGES CXX)
find_package(AnomalySDK CONFIG REQUIRED)
anomaly_add_plugin(my_plugin
  SOURCES plugin.cpp
  MANIFEST manifest.json
  PACKAGE_NAME MyPlugin)
```

`anomaly_add_plugin` always outputs `plugin.dll`, places the manifest and the `locales` directory next to the manifest into the package directory, and owns the install rule for that package: by default it installs to `<prefix>/Anomaly/plugins/<PACKAGE_NAME>` (the `GameRuntime` component). Two optional keywords change the install behavior: `NO_RELEASE` only builds and produces no install rule; `TEST_PLUGIN` installs into a separate `TestPlugins` component and only builds and installs when `ANOMALY_BUILD_TEST_PLUGINS=ON`. Any extra assets of your own (for example `data\`) still have to be listed explicitly in the project.

Source entry points: `<anomaly/sdk/anomaly_sdk.h>` (C) or `<anomaly/sdk/cpp.hpp>` (C++, which provides lightweight wrappers such as `Host`, `ServiceRef` and `UiWindow`).

## 3. Lifecycle and entry point

A minimal plugin that only displays text (adapted from [`examples/tick_counter`](../../../examples/tick_counter/plugin.cpp)):

```cpp
#include "anomaly/sdk/cpp.hpp"

namespace {
const AnomalyUiServiceV1* g_ui{};

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    const auto ui = anomaly::sdk::Host(host).Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    if (!ui) return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    g_ui = ui.get();
    *context = &g_ui;
    return anomaly::sdk::Ok();
}
AnomalyStatusV1 ANOMALY_CALL Start(void*) { return anomaly::sdk::Ok(); }
AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) { return anomaly::sdk::Ok(); }
void ANOMALY_CALL Unload(void*) { g_ui = nullptr; }
void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr) ui = g_ui;
    anomaly::sdk::UiWindow window(ui, "My Plugin");
    if (!window) return;
    ui->text(ui->user, anomaly::sdk::StringView("Hello from a plugin"));
}
}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor))
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.example.my-plugin"),
        anomaly::sdk::StringView("My Plugin"),
        anomaly::sdk::StringView("Me"), anomaly::sdk::StringView("1.0.0"),
        Load, Start, Stop, Unload, /*on_update*/ nullptr, Draw};
    return anomaly::sdk::Ok();
}
```

The lifecycle callbacks and their thread affinity:

| Callback | When | Thread domain |
| --- | --- | --- |
| `on_load` | After loading, query services and register assets | Lifecycle |
| `on_start` | Activation | Lifecycle |
| `on_update(delta)` | Logic update per tick | Game |
| `on_draw(ui)` | Drawing each frame | Render |
| `on_stop(deadline)` | Stop, commit state | Lifecycle |
| `on_unload` | Cleanup before unloading | Lifecycle |

Render callbacks must stay a pure drawing path. Game-semantic service queries, cursor advancement, paging, name resolution and statistics belong in the Game domain `on_update`, converted into the plugin's own immutable display snapshot; `on_draw` only copies the snapshot under a short lock and calls the UI outside the lock. [`nte_combat_demo`](../../../examples/nte_combat_demo/plugin.cpp) demonstrates that boundary: it resolves sources and participants only when new damage arrives, and idle frames do not call NTE services repeatedly.

See [Lifecycle and core services](../api-reference/lifecycle-and-core.md) for the complete entry / descriptor definitions.

## 4. Query services and degrade gracefully

Services are obtained through `query_service`, and all of them currently use version 1. The C++ wrapper `Host::Query` validates `struct_size`, `api_major` and `service_version` automatically, and returns an empty `ServiceRef` on failure:

```cpp
const auto config = anomaly::sdk::Host(host).Query<AnomalyConfigServiceV1>(
    ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION);
if (!config) { /* service unavailable, handle the degraded path */ }
```

> [!IMPORTANT]
> A plugin must handle the case where a service is unavailable. The UI service may not be ready yet; NTE services also return `UNAVAILABLE` when there is no active Profile or when the related Feature validation fails. Check `struct_size` before the call and the status after it.

An unauthorized service returns `ANOMALY_STATUS_V1_PERMISSION_DENIED`, which is distinct from `ANOMALY_STATUS_V1_UNAVAILABLE` for a service that is not published yet.

## 5. Manifest v2

Every plugin must have a `manifest.json`. Example:

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
    {"id": "anomaly.config", "minVersion": 1}
  ],
  "capabilities": ["ui", "configuration"]
}
```

See [Manifest and capabilities](../api-reference/manifest-and-capabilities.md) and [`schemas/plugin-manifest.schema.json`](../../../schemas/plugin-manifest.schema.json) for the complete field definitions.

- `id` is the normalized ID (dot-separated lowercase).
- `description` is the plugin blurb used in the "About" area of the management UI; a plugin can supply translations through `plugin.description` in its localization directory.
- `builds` is the build compatibility declaration, and it accepts either `nte-*` or an exact fingerprint. The current production Runtime does not compute fingerprints and does not use this field to block plugins on load, so it is not a runtime safety check.
- `license` is a single SPDX identifier (required for public distribution).

## 6. Capabilities and authorization

**Every non-optional required service must declare the matching capability**, otherwise the package is rejected before the DLL is mapped. Declaring an unknown capability, or having a required service that is not mapped / has no matching capability, makes the package unloadable. `anomaly.core` itself needs no capability.

See [Manifest and capabilities · capability mapping](../api-reference/manifest-and-capabilities.md#capability-mapping) for the capability ↔ service mapping.

Raw memory access is a **separate grant**:

- `read_memory` requires `memory-read`
- `write_memory` requires `memory-write`

When they are missing, the corresponding call returns `PERMISSION_DENIED`.

> [!NOTE]
> The capability mechanism constrains service visibility and resource ownership; it is **not** a security sandbox for native code in the same process. New plugins should prefer the semantic NTE snapshot services instead of reading game object memory themselves.

### Nearby pickup

When you need to pick up nearby `PropBox_`, `InteractBox_` or random item Actors, declare the `nte-pickup` capability and query `anomaly.nte.pickup`. Submit `AnomalyNtePickupRequestV1` in the Game Update and read `AnomalyNtePickupSnapshotV1` in Draw/UI. A request only means it has been queued; the Host maintains the confirmation count and the 2 second deadline, and you should not submit again while the state is `QUEUED` or `CHECKING`.

A plugin must not scan GObjects/World itself, parse `BPGetInteractEntries` or `BPCanTryInteract`, nor call UE `ProcessEvent` directly, modify UFunction flags or read `bInteractFinish`. Those behaviors belong to the NTE Adapter's Profile gate and thread domain, so that entity generation, object lifetime and confirmation logic do not fork across plugins. Reuse the entity cache and direct state bytes for confirmation first, and call `BPCanTryInteract` only once at the deadline.

### Clicking game UI buttons

When a flow needs to click game buttons, such as opening a screen or claiming a reward, declare the `nte-ui-buttons` capability and query `anomaly.nte.ui-buttons`. The flow is `request_scan` → wait for the request to reach `COMPLETE` → `find` the button by name / text / containing screen → `request_click` → wait for the request to reach `COMPLETE` and check `status`; chain multi-step operations in that order and rescan after each screen change. Before clicking, the Host re-determines whether the button is occluded by another screen; `CONFLICT` means it cannot be clicked at this moment, so wait or rescan rather than adding `FORCE` to click anyway.

A plugin must not scan GObjects to find buttons, call a button's UFunction directly or simulate a mouse click: calling the click directly bypasses Slate hit testing, and clicking an occluded button leaves the game in an unpredictable state. See [`anomaly.nte.ui-buttons`](../api-reference/nte-services.md#anomalynteui-buttons) for the service contract.

### Map landmark teleport

When you need to display every teleportable landmark on the map, declare the `nte-map-landmarks` capability and query `anomaly.nte.map-landmarks`. The Host builds the landmark catalog from the `TeleportPoint` DataTable validated by the active Profile; plugins only consume `TeleportID`, the owning world, the world position, the valid destination position, the floor and the type, and never scan GObjects, parse the DataTable or call `ProcessEvent` themselves.

The catalog uses `sequence` to identify an immutable version. Cache one complete enumeration in the plugin; re-read only when `sequence()` changes, and confirm that every `snapshot.sequence` returned by `snapshot_at()` still equals the target sequence. Store the selection state as `sequence + index`, and do not store UE pointers or reinterpret UI text as an ID:

```cpp
const auto sequence = service->sequence(service->user);
if (sequence != 0 && sequence != cached_sequence) {
    std::vector<AnomalyNteMapLandmarkSnapshotV1> next;
    const auto count = service->count(service->user);
    next.reserve(count);
    bool complete = true;
    for (std::uint32_t index = 0; index < count; ++index) {
        AnomalyNteMapLandmarkSnapshotV1 item{sizeof(item)};
        const auto status = service->snapshot_at(service->user, index, &item);
        if (status.code != ANOMALY_STATUS_V1_OK || item.sequence != sequence) {
            complete = false;
            break;
        }
        next.push_back(item);
    }
    if (complete && service->sequence(service->user) == sequence) {
        landmarks = std::move(next);
        cached_sequence = sequence;
    }
}
```

`on_draw` runs in the Render domain and only displays the options and puts the selected `sequence`, `index` and mode into the plugin's own pending state; `on_update` runs in the Game domain and, after confirmation in developer mode, builds an `AnomalyNteMapLandmarkTeleportRequestV1` and calls `teleport`. If the catalog has refreshed, the stale request returns `NOT_FOUND`, so the plugin refreshes the list and asks for a new selection instead of guessing the new index.

`plugins/TeleportLandmarksProbe` in the repository is the developer debug implementation of this pattern: it depends only on `anomaly.ui` and `anomaly.nte.map-landmarks`, and contains no signature, offset, raw memory read or UE call logic.

## 7. Resources and ownership

`anomaly.interop.signature` v1, in call domains that allow synchronous state I/O, automatically learns instructions that resolved successfully and tries to recover when the original signature matches nothing. The knowledge is stored in `signature-knowledge/signatures.json` in the plugin state directory, and the plugin's existing signature definitions, sources, manifest and C ABI need no changes. Game/Render callbacks skip this learning and recovery path; a plugin should resolve signatures during startup.
The original signature matching multiple times still returns `CONFLICT`, and insufficient evidence or a non-unique recovery still returns `NOT_FOUND`.

The raw signature interface only provides addresses and has no layout validator or field update contract, so it only wildcards RIP-relative addresses and relative branch operands, keeping object member and stack displacement bytes; wildcards already present in the original pattern still follow the original declaration. When the learning window matches nothing, the framework also tries to re-derive from the complete instructions of the original pattern, so no hand-written fallback pattern is needed.
Compile-time field offset constants in existing DLLs cannot be replaced by signature recovery; layout changes outside the window or not constrained by the original signature cannot be validated by the address resolution interface either. Plugins that bypass the service and scan on their own do not go through this recovery path.

Plugins that need automatic field offset learning can opt into signature v2 and include `anomaly/sdk/signature.hpp`. Set `minVersion` to `2` for that service in the manifest. Signatures and semantic seeds still live in the plugin's own header; call the framework API once during startup, and later callbacks only read the local snapshot. For example, with an existing `kPausePattern` / `kResumePattern`:

```cpp
inline constexpr AnomalySignatureFieldV2 fields[]{
    anomaly::sdk::FieldSeed("PlayingID", 0x438, 4),
    anomaly::sdk::FieldSeed("Paused", 0x43C),
};
inline constexpr AnomalySignatureMemberV2 members[]{
    anomaly::sdk::MemberUse(0, 4, ANOMALY_SIGNATURE_MEMBER_READ_V2),
    anomaly::sdk::MemberUse(1, 1, ANOMALY_SIGNATURE_MEMBER_WRITE_V2),
};
inline constexpr AnomalySignatureWitnessV2 witnesses[]{
    anomaly::sdk::Witness("pause", kPausePattern, members),
    anomaly::sdk::Witness("resume", kResumePattern, members),
};
inline constexpr anomaly::sdk::LayoutSchema schema{
    "HTGame.exe", ".text", fields, witnesses};

// Lifecycle / Worker; signature is the AnomalySignatureServiceV2 you already queried.
auto resolved = anomaly::sdk::ResolveLayout(signature, schema);
if (resolved) {
    const auto& layout = resolved.value();
    const auto pause_address = layout.address("pause");
    const auto paused_offset = layout.offset("Paused");
    // Store the address and offsets in the plugin Context; later field access uses paused_offset.
}
// On failure, report the reason with resolved.error() and disable the features that depend on this group of layouts.
```

The framework validates the object source, access width, alignment, unique match and consistency across multiple extraction points, and only publishes and learns the fields once the whole group agrees.
A function a game update rewrote so that neither its signature nor a learning record locates it is published as address zero while the remaining fields and addresses still resolve: declaring a second, independent function for the same field (another read site beside `pause`) keeps that field valid when one function is lost, and a hook that needs the missing site resolves it again itself and skips only if that also fails.
Seeds are not failure fallback values. See
[Interop and memory](../api-reference/interop-and-memory.md#version-2-resolve-addresses-and-field-layouts-together) for the complete restrictions, C ABI output ownership and error codes.
HiFi already integrates PlayingID, Paused, PlayerType, CurrentID and OwnedIDs; core playback and the optional album grouping are resolved, and an album group failure keeps the existing album degraded behavior. Other fields still follow the existing compatibility constraints.

Every handle created by Platform / Interop / IPC is **bound to a plugin generation**:

- Scheduler / IPC work that has not started is cancelled on stop / reload.
- Tracked patches restore the original bytes on release / stop / reload; recoverable patches must use `anomaly.interop.patch`.
- A function hook's detour must be wrapped in `begin_callback` / `end_callback`; hooks currently support the function kind only.
- After a plugin callback fails for the first time, later calls stop; a failed stop generation stays isolated.

Memory allocated by the host is freed by the host allocator; the public boundary passes no C++ STL, exceptions, `ImGuiContext*`, raw `UObject*` or host-internal objects.

## 8. Local development loop

```powershell
cmake -S . -B build -DAnomalySDK_DIR=C:\AnomalySDK\lib\cmake\AnomalySDK
cmake --build build --config Debug
anomaly-plugin validate build\package\MyPlugin
anomaly-plugin pack build\package\MyPlugin --output .\dist
anomaly-test-host --plugin .\dist\anomaly.example.my-plugin --reload 10 --ticks 60
```

`anomaly-test-host` is an offline **game-free** fixture: it loads your package, runs a number of ticks and reloads, and verifies lifecycle and degraded behavior. That environment has no real game adapter, so NTE services stay `UNAVAILABLE`, but the plugin package itself must still load and unload cleanly.

## 9. Deployment and hot reload

Copy a package that passes TestHost into `Anomaly\plugins\<package directory>`. File watching (with debounce) will:

1. validate the new generation first;
2. stop the old Scope;
3. reload **only the changed package and the downstream packages in its dependency closure**.

**Reload all** in the management UI triggers a full reload explicitly. Before activation it pre-checks static / supported delay-load imports, private CRT / system DLLs and already-loaded modules with the same name; modules loaded dynamically via `LoadLibrary` are outside the pre-check, so a plugin must avoid process-level DLL name conflicts itself.

## 10. Distribution

- Fill `license` in the manifest with a single SPDX identifier; provide separate licenses for assets or native dependencies with the package.
- Both Debug / Release must satisfy the same ABI contract.
- Prefer the semantic snapshot services; when raw memory is genuinely required, declare `memory-read` / `memory-write` explicitly.

To let users install directly from **Plugins > Available**, zip the directory package with the correct root structure and publish `pluginmaster.json`. See [Distributing third-party plugins](plugin-distribution.md) for the fields, publishing order and local `file://` testing.

## Examples at a glance

| Example | Demonstrates |
| --- | --- |
| [`hello_ui`](../../../examples/hello_ui/plugin.c) | Pure C scoped Window / Font / Texture / Input / Hotkey with host-owned UI; degraded drawing when assets are not ready |
| [`tick_counter`](../../../examples/tick_counter/plugin.cpp) | C++ RAII + Game Thread Tick |
| [`reliable_config`](../../../examples/reliable_config/plugin.cpp) | Registering a JSON Schema through the Config ABI, reading settings, atomic commit in `on_stop` |
| [`nte_inspector`](../../../examples/nte_inspector/plugin.cpp) | Session lifecycle events, bounded EntityPage, class-name resolution and Host snapshot metrics |
| [`nte_combat_demo`](../../../examples/nte_combat_demo/plugin.cpp) | Incremental character damage event consumption in the Game domain, with the Render domain drawing only the local snapshot |

## Related

- [API reference](../api-reference/README.md) — complete interfaces
- [Distributing third-party plugins](plugin-distribution.md) — ZIP, plugin catalog and the publishing flow
- [Architecture overview · ABI boundary](architecture.md#abi-boundary)
- [Contributing guide](contributing.md)
