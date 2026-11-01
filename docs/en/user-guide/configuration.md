# Configuration reference

Anomaly's platform configuration lives in `Anomaly\anomaly.ini`. Signatures and structure offsets are **not** in the INI — they belong to [NTE Build Profile](nte-profiles.md); `anomaly.ini` only selects the game, the Profile directory and platform behavior.

## All anomaly.ini fields

```ini
[Analyzer]
PipePrefix=Anomaly
MaxScanResults=1024

[Platform]
Enabled=1
Visible=0
Embedded=1
AttachToProcessWindow=1
ToggleKey=45
Language=auto
PluginDirectory=plugins

[Performance]
UpdateSlowMilliseconds=2
DrawSlowMilliseconds=4
PlayerSnapshotTickInterval=1
EntitySnapshotTickInterval=1
ActorSnapshotTickInterval=60

[Profiles]
Game=nte
Directory=profiles
LocalDirectory=profiles-local
ManagedDirectory=state/profiles/managed
```

### [Analyzer]

| Field | Description |
| --- | --- |
| `PipePrefix` | Named pipe name prefix used by the diagnostics protocol. |
| `MaxScanResults` | Maximum number of hits returned by a single signature scan. |

### [Platform]

| Field | Description |
| --- | --- |
| `Enabled` | Whether the platform (UI and plugin host) is enabled. |
| `Visible` | Whether the interface is visible at startup, defaults to `0`; press `Insert` to summon it manually. |
| `Embedded` | `1` enables D3D12 swapchain embedded mode (the in-game interface). |
| `AttachToProcessWindow` | Takes effect only when embedded mode is off and the Win32 fallback backend is used; the standalone demo program always uses a normal window. |
| `ToggleKey` | Win32 virtual-key code that summons / collapses the interface. The default `45` is `VK_INSERT`; changing the key directly in **Anomaly Launcher > Startup settings** is recommended. |
| `Language` | UI language, see below. |
| `PluginDirectory` | Plugin directory (relative to `Anomaly\`), defaults to `plugins`. |

### [Performance]

| Field | Description |
| --- | --- |
| `UpdateSlowMilliseconds` | Threshold (milliseconds) for classifying a plugin `on_update` callback as "slow". |
| `DrawSlowMilliseconds` | Threshold (milliseconds) for classifying a plugin `on_draw` callback as "slow". |
| `PlayerSnapshotTickInterval` | Sampling interval (ticks) for the player snapshot. |
| `EntitySnapshotTickInterval` | Sampling interval (ticks) for entity snapshots. |
| `ActorSnapshotTickInterval` | Rescan interval (ticks, default 60) for the whole-level actor snapshot. Raise it to cut scan cost, lower it so destroyed or newly spawned actors are reflected in the snapshot sooner. |

### [Profiles]

| Field | Description |
| --- | --- |
| `Game` | Current game identifier (such as `nte`). |
| `Directory` | Profile directory shipped with the runtime package. |
| `LocalDirectory` | Local Profile override directory with the highest priority. |
| `ManagedDirectory` | Profile directory delivered by the update source. |

The Runtime selects in this order: local override > delivered by the update source > shipped with the runtime package. See [NTE Build Profile](nte-profiles.md) for details.

## Language

`Language` accepts three values:

- `auto` — selected from the Windows user language when the process starts.
- `en-US` — English.
- `zh-CN` — Simplified Chinese.

> [!NOTE]
> After changing `Language`, you must **restart the affected process** for it to take effect.

## Toggle key

`ToggleKey` uses Win32 virtual-key codes. Common values:

The launcher shows the current hotkey and captures the next keypress, saving it to runtime settings; editing `anomaly.ini` by hand stays supported and takes effect after the Runtime restarts.

| Key | Decimal code |
| --- | --- |
| `Insert` (default) | `45` |
| `Home` | `36` |
| `End` | `35` |
| `F1` | `112` |

For the full set of key codes, see Microsoft's [Virtual-Key Codes](https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes) documentation.

## Interface layout

The in-game main interface is a draggable, resizable ImGui floating window; its position and size are saved in `Anomaly\anomaly-imgui.ini`:

- Centered at `1040×700` on first display.
- The main management window has no close button; a closed state left behind by an older version is repaired automatically at startup.
- `Insert` collapses / expands unlocked menus; the lock button in the title bar lets a single window stay expanded.

### Theme and background image

The interface color scheme is picked in **Settings > Interface > Color palette** (the built-ins include Naiwa / Milk Frog, **and Naiwa is the default**), while background images and stickers are written in `Anomaly\config\themes\` (`layout.json` is generic, `<palette name>.json` is palette-specific); that directory ships with only the Naiwa preset (`naiwa.json` and its stickers), and the other layouts are generated only the first time you change a background or sticker. Both apply to the in-game main interface and to the launcher.

Full fields, the three background fill modes and how to use the web sticker editor are in [Theme and background](theme.md).

## Third-party plugins (plugin-repositories.json)

For the day-to-day steps of adding, downloading, updating and uninstalling plugins, see [Third-party plugins](third-party-plugins.md). This section covers only the configuration file format.

`Anomaly\plugin-repositories.json` stores the plugin source addresses used in **Plugins > Third-party plugins**. The runtime package ships one enabled GitHub index; the exact address is whatever the JSON in the runtime package contains. Plugin sources and download addresses must use HTTPS by default. `file://` is for local testing only and requires explicitly setting `"allowInsecureSources": true`.

```json
{
  "schemaVersion": 1,
  "enabled": true,
  "allowInsecureSources": false,
  "repositories": [
    {
      "url": "https://example.com/pluginmaster.json",
      "enabled": true
    }
  ]
}
```

After you apply plugin sources in the interface they refresh immediately, with no Runtime restart needed. If a network refresh fails, the interface still shows the list from the last successful fetch. Installation downloads the plugin package again and checks the ZIP path, manifest ID, version, game, Plugin API and entry DLL.

Plugins installed from a plugin source can be uninstalled in the action menu. Uninstalling removes only the plugin package; settings in `Anomaly\config\plugins\` are kept.

## Signed update source (repository.json)

`Anomaly\repository.json` is the Profile signing update source used by the Runtime. The current runtime package ships with this feature off (`"enabled": false`) and with no preset update address or trust key:

```json
{
  "schemaVersion": 1,
  "enabled": false,
  "allowFileSources": false,
  "withdrawalPolicy": "block-new",
  "freshness": {
    "maximumClockSkewSeconds": 300,
    "maximumIndexAgeSeconds": 86400,
    "maximumOfflineAgeSeconds": 604800,
    "downgradePolicy": "reject"
  },
  "sources": [],
  "trustKeys": []
}
```

To enable it you must configure both `sources` and `trustKeys`. Regular users do not need to edit this file.

## Related

- [NTE Build Profile](nte-profiles.md) — signatures and structure offsets
- [Persisting plugin settings](../developer-guide/plugin-development.md) — a plugin's own JSON configuration is stored in `Anomaly\config\plugins\`
