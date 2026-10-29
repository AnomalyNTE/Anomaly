# User documentation

> 🌐 English · [简体中文](../../user-guide/README.md)

This is how you use Anomaly day to day: how to install it, where to find plugins, how to update, and what to check first when something goes wrong. If you want to build from source or write your own plugins, go to the [developer guide](../developer-guide/README.md).

## Reading order

1. [Installation and updates](installation.md) — download the right runtime package and use the launcher for the first install, an update, or Live attach.
2. [Quick start](quickstart.md) — launch for the first time, open the interface, and enable plugins.
3. [Built-in plugins](built-in-plugins.md) — what each plugin installed with the runtime package does.
4. [Third-party plugins](third-party-plugins.md) — download, update, and uninstall plugins in game, and manage plugin sources.
5. [Troubleshooting and FAQ](troubleshooting.md) — common problems such as the interface not appearing, failed downloads, and plugins not loading.
6. [Configuration reference](configuration.md) — change the toggle key, the language, the Profile directory, and the plugin source.
7. [Themes and backgrounds](theme.md) — swap color schemes (including the Milk Frog theme), apply background images and stickers, and build layouts with the web editor.
8. [Diagnostics CLI](diagnostics-cli.md) — inspect Runtime, Profile, and memory state with `anomaly-cli`.
9. [NTE Build Profile](nte-profiles.md) — why features that depend on the game's structures may be temporarily unavailable.

## Terminology quick reference

| Term | Meaning |
| --- | --- |
| **Runtime / Core** | The `Anomaly.Core.dll` injected into the game process, which carries every platform capability. |
| **Proxy** | The `dwmapi.dll` in the root directory, which forwards system exports and bootstraps Core. |
| **Platform** | The host UI and plugin management layer inside the Runtime. |
| **Profile** | The set of game symbols, structure offsets, and Feature dependencies. |
| **Feature** | A capability that depends on validated symbols (such as player position or entity snapshots); when that does not hold, the Feature degrades to unavailable. |
| **Plugin** | A directory package containing `manifest.json` and `plugin.dll`, loaded by the Runtime and hot-reloadable. |
| **Plugin source** | A `pluginmaster.json` address that provides the list of downloadable third-party plugins. |

> Read the disclaimer in the root [README](../../../README.en.md) before use.
