# Contributing guide

Contributions are welcome. This page summarizes the workflow and the boundaries that cannot be crossed. **The authoritative source for the collaboration conventions is [`AGENTS.md`](../../../AGENTS.md) in the repository root**; read it in full before you submit a change.

## Delivery workflow

1. Run `git status --short --branch` before you start, and keep modified and untracked files that are unrelated to the current task.
2. Before editing, determine the **module and public boundary** the feature belongs to. Meet the requirement with the smallest change; do not carry unrelated refactoring or formatting.
3. A behavior change must add or update validation in the **layer it belongs to**; run the narrowest relevant validation first, then widen the scope according to the impact.
4. Run `git diff --check` before you finish, review the staged diff, and state which validation you actually ran or why you did not run it.
5. Every source / build / CI / Profile / Schema / runtime behavior / tooling script change needs a corresponding **atomic Git commit**. Documentation-only changes should also be committed unless you are explicitly asked not to.

## Commit conventions

Use the **Conventional Commit** style:

```text
fix(plugin): drain callbacks before unload
build(cmake): unify the local preset entry point
docs(sdk): document the config schema migration flow
```

- Stage explicit files only; **`git add -A` is forbidden**.
- Do not rewrite other people's commits, and do not use `reset --hard` or a forced checkout to hide work.
- A commit may only contain files from the current task. When you cannot create the expected commit, you must state the blocking reason explicitly.

## Code boundaries that cannot be crossed

The following rules come from [`AGENTS.md`](../../../AGENTS.md) and are the floor for architectural integrity:

- `src/bootstrap/dwmapi/` must work **outside the loader lock**: it only forwards system exports and starts Core asynchronously, and it holds no configuration, hooks, UI, scanning, or plugin loading.
- `src/runtime/` owns session state, stop propagation, service lifecycle, and Dispatcher ownership. When part of startup fails, services that already started must still stop in **reverse** dependency order.
- `src/platform/`, `src/services/`, and `src/diagnostics/` stay **game-agnostic** and do not depend on the NTE layout.
- `src/game/ue5/` owns validated generic UE symbols; `src/game/nte/` selects the active Profile and Feature Gate by source priority. New offsets / signatures require a **Profile, a validator, and degrade behavior**, and cannot be hardcoded in a plugin or the UI.
- `src/plugin/` owns the Catalog, packages, generation, Scope, hot reload, and fault isolation. Every resource visible to a plugin is recorded in `PluginScope`, and a callback never runs past its generation lease.
- Render callbacks in `src/render/dx12/` and `src/ui/` **must not** scan packages, load DLLs, wait on lifecycle tasks, or perform synchronous I/O.
- `include/anomaly/sdk/` is the public DLL boundary: pure C ABI only, with fixed-width types, opaque handles, clear ownership, and version / `struct_size` validation. Do not expose STL, exceptions, RTTI, raw UE objects, `ImGuiContext*`, or internal headers.

For the complete module, threading, and change-impact map, see the [architecture overview](architecture.md) and `.agents/architecture.md`.

## Build and validation

The main repository uses the `windows-vs2022` configure preset and the `windows-relwithdebinfo` build preset. `build.cmd` is only a convenience wrapper around the same command sequence CI runs. Creating a temporary NMake / Ninja / `build/` or phase-named main build tree is **forbidden**. For commands, see [Building from source](building.md).

## Live targets and data safety

> [!CAUTION]
> Live process tools are for explicit diagnostic tasks only and cannot be used as everyday validation. `write` / `patch` / `protect` / `alloc` / `free` in `anomaly-cli` modify other processes, require explicit authorization, and the target **must be a test process you own**. Profile rescans and entity scans are mostly reads, but they still need an explicit request and an active Profile context.

Logs, dumps, Profile data, and diagnostic bundles may contain sensitive information; review and redact them before sharing.

## Documentation

- User-visible behavior changes should update the [user documentation](../user-guide/README.md).
- Public interface changes should update the [API reference](../api-reference/README.md).
- Documentation describes stable interfaces and workflows; volatile numbers (fingerprints, release manifests) stay in the code and build output and are not written into documentation, to avoid drift.
