# Troubleshooting and FAQ

Find the section that matches the symptom. The logs are the most important entry point for diagnosis:

- `Anomaly\logs\anomaly-runtime.log` — Runtime and pipe service log (PID, state, pipe name).
- `Anomaly\logs\anomaly-runtime.jsonl` — structured log, rotated by size, where `anomaly-runtime.jsonl.1`…`.3` are the historical files.
- `Anomaly\logs\nte_game_hook.log` — startup injection and hook installation record.
- `Anomaly\anomaly-platform.log` — platform and plugin log.

Logs do not need manual cleanup: logs whose last write is older than 3 days, and the oldest logs that push their directory beyond 48 MiB, are deleted automatically (once when the Runtime starts, then once per hour while it runs). When you hit a problem and need to keep the evidence, copy the logs out early; do not count on them staying in place.

## Install / launcher

### Install still fails after you select a directory

- You selected the **folder that directly contains `HTGame.exe`**, not the `Neverness To Everness` install root.
- `AnomalyLauncher.exe`, `dwmapi.dll` and `Anomaly\Anomaly.Core.dll` must come from the same fully extracted runtime package. Moving only the launcher makes the install source unavailable.
- Exit the game before updating so that the old files are not still held by a process.

### The launcher reports "Conflict"

The game directory may already contain another tool's `dwmapi.dll`, or `dwmapi.dll` and `dwmapi.dll.disabled` may exist at the same time. The launcher does not overwrite or delete these files for you. First work out which tool the file belongs to, then decide which one to keep; Anomaly and RE-UE4SS cannot share a proxy of the same name.

### "Start and attach" is grayed out

The **Start and attach** option under Live attach only handles the game process that the launcher creates this time: it stays disabled while the page already lists a running `HTGame.exe`. In that case use **Inject into running process** instead; if you want to go through the startup flow again, exit the game first.

### "Inject into running process" is grayed out

It requires the page to actually list an attachable `HTGame.exe` and to be able to resolve the Runtime. First click the refresh button next to the status row to scan again; if the listed process is marked as not attachable, it is usually a privilege mismatch (the game was launched as administrator) or the process is not x64.

### Injection into the running process fails

The failure message includes the Win32 error code and the specific stage. The game is not restarted or terminated, so you can try again, or switch to **Start and attach** to go through the new-process path. Common causes are the game running with higher privileges (run the launcher as administrator), or the game side never letting a new or existing thread reach an insertable state.

## Interface / injection

### Pressing `Insert` does nothing and the interface does not appear

1. Confirm that the Runtime is injected: were the log files above created and do they record a PID?
2. Confirm the toggle key: check **Anomaly Launcher > Startup settings** first; when configured manually it is `[Platform] ToggleKey` in `anomaly.ini` (a Win32 virtual-key code, default `45` = `Insert`).
3. Confirm the platform is enabled: `[Platform]` should have `Enabled=1` and `Embedded=1`. `Visible=0` is a normal default and only means the interface starts hidden.
4. Confirm that `dwmapi.dll` sits in the **directory of the main game EXE** and has not been renamed to `dwmapi.dll.disabled`.
5. Check for a conflict with RE-UE4SS (see below).

### D3D12 Present errors after enabling DLSS / frame generation

The embedded backend converts the Streamline proxy device and command queue to native D3D12 interfaces before submitting its own commands, and preserves the swap chain's own buffer access. A submission queue observable when the swap chain was created takes precedence over the global queue capture; when attaching live to an already created swap chain, the direct queue observed by the current Present thread takes priority so that a background frame-generation thread does not override it. Nested `Present` / `Present1` draws the platform interface only once.

These handling paths do not mean that every DLSS / driver combination has been tested. When investigating, verify DLSS super resolution and frame generation separately, as well as the behavior after toggling features or changing resolution at runtime. `E_ABORT` only means that the present chain returned an aborted operation; it does not identify the failing component on its own. Combine it with `Anomaly\\logs`, `Anomaly\\crashes` and the game crash report.

To temporarily bypass the embedded present chain, set `[Platform] Embedded` in `Anomaly\\anomaly.ini` to `0`, keep `AttachToProcessWindow=1`, exit the game and start it again. This uses the separate-window overlay backend; its input and fullscreen coverage behavior differ from swap chain embedded mode.

### No logs are generated / the Runtime is not injected

- Confirm that you are using Anomaly's `dwmapi.dll` (not the system one).
- In proxy mode, the game must actually load `dwmapi.dll`.
- Live attach: **if the game restricts process handles from creation time, use proxy install instead**.

### Conflict with RE-UE4SS

Anomaly's proxy and RE-UE4SS's bootstrap DLL **have the same name** (both are `dwmapi.dll`), so they cannot share one directory. Only one of them can be present. Anomaly does not load `UE4SS.dll`.

## Third-party plugin download and updates

### The "Available" page is empty

- Open **Plugins > Third-party plugins** and confirm that the master switch and at least one plugin source are enabled, and that the address uses HTTPS.
- Click **Apply**, return to **Available**, then click refresh in the top-right corner.
- When the plugin source is offline only the last cached list may remain; on first use with no cache, the page stays unavailable.
- A plugin may also be missing because it lacks a required field in `pluginmaster.json`, or because its ID / SemVer format is invalid.

### Installation or update fails

- Read the failure reason under the plugin row first, then `Anomaly\anomaly-platform.log`.
- `plugin package URL must use HTTPS`: the download address is not HTTPS, or the plugin source returned an invalid address.
- `manifest id/version does not match`: the plugin catalog and the manifest inside the ZIP were not updated together.
- `package has no manifest.json at its root`: the ZIP has an extra directory level.
- `manifest plugin API does not match` / `not published for this game`: the plugin does not support the current Runtime or game.
- When a missing dependency is reported, install the dependency plugin separately from **Available**; the installer does not download dependencies automatically.

For full usage see [third-party plugins](third-party-plugins.md).

## Plugins

### A plugin does not load

- A plugin must be a **directory package containing `manifest.json`**. Root-level DLLs and directories without a manifest are not loaded.
- Check whether the manifest passes schema validation, and confirm that the required services and capability declarations are complete.
- Check the **status reason** for the plugin on the **Plugins** page; common rejection reasons are:
  - **unknown capability** — the manifest declares an unknown capability.
  - **required service missing mapping / capability** — a non-optional required service has no matching capability declaration.
  - **DLL preflight conflict** — a module with the same name is already loaded, or there is a native dependency conflict that cannot be isolated.

For the mapping between capabilities and services see [plugin development](../developer-guide/plugin-development.md) and [Manifest and capabilities](../api-reference/manifest-and-capabilities.md).

### The plugin loaded, but some features are unavailable / grayed out

This is usually a [Feature degradation](nte-profiles.md#feature-degradation): the active Profile lacks a required declaration, or symbol, layout or ABI validation did not pass. The plugin still loads; only the affected features stay `unavailable`.

Use the CLI `ue` command or **Diagnostics > Developer > NTE Compatibility** to inspect the Feature Matrix.

### Behavior is wrong after reloading plugins

- When one package changes, only that package **and the downstream packages that depend on it** are reloaded; for a full reload use **Reload all**.
- If a generation times out during drain and is quarantined, its DLL stays mapped until the process exits. This is intentional safety design (executing code that has already been unmapped is more dangerous).

## CLI / diagnostics

### `anomaly-cli` cannot connect

- Confirm that `--pid` is the PID of the **game process** (see `anomaly-runtime.log`).
- The pipe is restricted by the current user's ACL: the CLI must run as the **same Windows user**.
- Confirm that `[Analyzer] PipePrefix` matches expectations.

### A command returns error

`error.code` / `error.message` in the response are stable error identifiers. Memory write commands require the target address page to be writable / present.

## Configuration

### `Language` changed but nothing happened

A `Language` change takes effect only after you **restart the corresponding process**. The only valid values are `auto`, `en-US`, `zh-CN`.

### Persistent plugin settings are missing

JSON settings saved by plugins through `anomaly.config` live in `Anomaly\config\plugins\`; global enablement state lives in `Anomaly\config\plugin-enablement.json`.

## Crashes

- An unhandled exception produces a `MiniDumpNormal` plus sidecar metadata.

> [!CAUTION]
> Diagnostic bundles, crash dumps and Profile data may contain sensitive information; review and redact them before sharing.

## Still stuck?

- Review [installation and updates](installation.md), [third-party plugins](third-party-plugins.md) and [configuration reference](configuration.md).
- For development / build questions see the [developer documentation](../developer-guide/README.md).
