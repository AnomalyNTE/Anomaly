# Quickstart

This page assumes you have already completed the proxy install as described in [Installation and updates](installation.md), or that you plan to use live attach from the launcher. It starts at your first time entering the game.

## 1. Start the game

When the proxy is already enabled, just start the game as usual. With live attach, exit any running `HTGame.exe` first, then click **Start and attach** on the **Live attach** page of `AnomalyLauncher`. Once Runtime enters the game process it writes two logs:

- `Anomaly\logs\anomaly-runtime.log` — Runtime state, PID, named pipe name.
- `Anomaly\anomaly-platform.log` — platform and plugin logs.

If these logs appear after the game starts and record the PID, Runtime injected successfully.

## 2. Bring up the management interface

Press **`Insert`** to show or collapse the management interface. It is a draggable, resizable ImGui floating window, centered at `1040×700` on first display; its position and size are saved in `Anomaly\anomaly-imgui.ini`.

The main management interface always has three main navigation entries:

| Page | Content |
| --- | --- |
| **Plugins** | Manage installed plugins, browse and update third-party plugins, and maintain plugin sources. |
| **Diagnostics** | View Runtime state; the **Developer** area also has service, Hook, memory and NTE compatibility information. |
| **Settings** | Platform and plugin settings (language, toggle key, and so on). |

> [!NOTE]
> The main management window has no close button, so you cannot lose the entry point for opening the platform again. `Insert` collapses or expands unlocked menus and also recovers a main window that closed abnormally; the lock button in the title bar keeps a single window expanded.

## 3. Enable built-in plugins

The **Plugins** page shows the built-in plugins installed with the runtime package. Select a plugin to enable or disable it, reload it, or inspect its status reason.

On a fresh install all plugins are off and do not load automatically with Runtime; enable them one by one as needed. When you enable a plugin that has required dependencies, Runtime enables its dependencies too.

- Plugin windows are controlled through **Open / Hide** in the management interface; after you close a plugin window with its X, you can reopen it from here.
- Each plugin is a separate directory package; root-level DLLs and directories without a manifest are not loaded.

See [Built-in plugins](built-in-plugins.md) for what each built-in plugin does.

> [!TIP]
> Coordinates, Entity ESP and teleport all depend on a Profile. If they show as unavailable or degraded, first check the symbol and Feature state in [NTE Build Profile](nte-profiles.md).

## 4. Install third-party plugins

Open **Plugins > Available**, find the plugin, and click **Install**. When the install finishes it appears under **Installed** and stays off; enable it manually after you have read its description and status.

When a plugin has a new version it appears under **Updates**. To add or disable a plugin source, open **Third-party plugins**. See [Third-party plugins](third-party-plugins.md) for detailed steps and security reminders.

## 5. Switch back to the game

After you switch back to the game (hiding or collapsing the interface), no Anomaly window receives mouse input, and collapsing the menu does not affect a plugin's foreground drawing (an ESP overlay still renders, for example).

## 6. (Optional) Diagnose with the CLI

Runtime exposes a named pipe diagnostic protocol. The control client `anomaly-cli.exe` can live in any directory and connects through the PID:

```powershell
.\anomaly-cli.exe --pid <PID> status
.\anomaly-cli.exe --pid <PID> modules
```

See [Diagnostics CLI](diagnostics-cli.md) for the full command set.

## Next steps

- [Configuration reference](configuration.md) — change the toggle key, language, performance items
- [Built-in plugins](built-in-plugins.md) — what each built-in plugin does
- [Third-party plugins](third-party-plugins.md) — download, update and manage plugin sources
- [Troubleshooting and FAQ](troubleshooting.md) — common issues with the interface, plugins and Features
