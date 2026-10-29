# Installation and updates

Regular users do not need to build from source. Download the Runtime ZIP, extract it completely, then run the launcher. For a first install, use "Proxy install"; "Live attach" suits temporary scenarios where you do not want proxy files in the game directory.

> [!CAUTION]
> Anomaly enters the game process and reads or modifies runtime state. Read the disclaimer in the root [README](../../../README.en.md) before use.

## Which package to download

The release page usually offers several ZIPs at once. For everyday use you only need:

```text
Anomaly-<version>-runtime.zip
```

`sdk.zip` is for plugin developers and `tools.zip` holds the command-line tools; neither replaces the Runtime.

Extract the Runtime ZIP **completely** into an ordinary folder — do not run it straight from the archive, and do not pull out only `AnomalyLauncher.exe`. When installing, the launcher also reads the `dwmapi.dll` and `Anomaly\` directory next to it.

The extracted package contains these main files:

```text
AnomalyLauncher.exe          Install, update, and live attach
dwmapi.dll                   Proxy entry used when the game starts
Anomaly\
  Anomaly.Core.dll           Runtime main program
  anomaly.ini
  repository.json
  plugin-repositories.json
  assets\
  locales\
  profiles\
  plugins\                   Built-in plugin packages shipped with the Runtime
```

When you build from source, the equivalent directory is `.build\windows-vs2022\game-package`. See [Building from source](../developer-guide/building.md) for the exact steps.

## Recommended: install with the launcher

1. Exit the game first. When updating an older version, also close any `AnomalyLauncher.exe` still running.
2. Run the `AnomalyLauncher.exe` you just extracted. A UAC prompt is normal — the launcher needs administrator rights to write into the game directory.
3. Stay on the **Proxy install** page at the top.
4. Check "Game directory". If the auto-detected path is wrong, click the folder button and pick **the folder that directly contains `HTGame.exe`**, not the game installation root.
5. Once the status shows "Not installed", click **Install**. Wait until the status becomes "Enabled" before you close the launcher.
6. Start the game as usual, and press `Insert` after entering to bring up the Anomaly interface.

The game directory is usually:

```text
Neverness To Everness\Client\WindowsNoEditor\HT\Binaries\Win64\
```

After installation it contains at least:

```text
Win64\
  HTGame.exe
  dwmapi.dll
  Anomaly\
    Anomaly.Core.dll
```

`AnomalyLauncher.exe` does not need to be copied into the game directory. Starting the game does not depend on the original extracted folder either; you can keep it for future updates, or re-extract a new Runtime whenever you need one.

A successful start produces two log files:

- `Anomaly\logs\anomaly-runtime.log`: whether Core started, the game PID, and diagnostic pipe information.
- `Anomaly\anomaly-platform.log`: interface, plugin, and platform service logs.

The structured logs under `Anomaly\logs\` roll over by size, and old log files are deleted automatically (written more than 3 days ago, or once the directory exceeds 48 MiB), so you never need to clean them up by hand. By the way: copy out any log you want to keep in time.

If pressing `Insert` does nothing, first check whether these two files were created, then continue with [Troubleshooting](troubleshooting.md).

## Updating to a new version

Do not extract the new ZIP over the old extracted folder. Extract the new Runtime completely into a new folder, then:

1. Exit the game.
2. Run the new `AnomalyLauncher.exe` and confirm it selected the directory that contains the correct `HTGame.exe`.
3. Once the status shows "Update available", click **Update**.

The launcher compares the SHA-256 of the old and new `Anomaly.Core.dll`. "Update available" appears only when Core differs. During the update it keeps:

- `anomaly.ini`, the repository configuration, and the user settings under `config\`;
- runtime data in `state\`, `logs\`, and `crashes\`;
- plugins you added yourself and other files that are not part of the new runtime package;
- whether the proxy is enabled or disabled.

The Core, Profiles, and built-in plugins shipped with the new version are replaced with the ones from the new package. That means built-in files you modified yourself can be overwritten by an update; changes you want to keep long term belong in a custom plugin or in local configuration.

> [!NOTE]
> Installing and updating switch over through a temporary directory and a backup directory. If an operation fails, the launcher restores the original installation as far as it can; you do not need to delete the old version by hand first.

## Temporarily disabling the proxy

Click **Disable** on the **Proxy install** page and the launcher renames:

```text
dwmapi.dll
```

to:

```text
dwmapi.dll.disabled
```

Click **Enable** when you want it back. This only affects the **next** start; it does not unload a Runtime that has already entered the current game process. Exiting the game before switching is recommended.

## Manual copy without the launcher

A clean install also works by hand: exit the game and copy `dwmapi.dll` and the entire `Anomaly\` directory from the Runtime package next to `HTGame.exe`.

If the target directory already has a `dwmapi.dll`, first establish who it belongs to. Anomaly does not share that location with another proxy of the same name, and it should not overwrite files of unknown origin. For updating an existing installation, let the launcher handle it, so that user configuration and custom plugins are not caught by a plain file overwrite.

## Live attach

**Live attach** does not copy `dwmapi.dll` or `Anomaly\` into the game directory. It manually maps the Core from the current runtime package into `HTGame.exe`, with either timing:

- **Start and attach**: the launcher starts the official `NTELauncher.exe`, waits for the `HTGame.exe` created by that start, and then maps Core.
- **Inject into running process**: a game that is already running (started from its own shortcut, or not picked up by the launcher last time) can be attached afterwards. The launcher only maps Core and does not restart the game; on failure the game keeps running untouched.

To use it:

1. Keep the entire runtime package structure intact.
2. Run `AnomalyLauncher.exe` and switch to **Live attach**.
3. If the official launcher was not detected automatically, select `NTELauncher.exe` manually.
4. Click **Start and attach** to take over this start; if the game is already running, click **Inject into running process**, then continue playing through the official launcher's normal flow (or the existing window).

The two buttons complement each other: when the page already lists a running `HTGame.exe`, "Start and attach" is not enabled (it would spawn a second process), so use "Inject into running process"; conversely, when no game is running, "Inject into running process" is not enabled.

When attaching to a game that is already running, the launcher tries the ways of entering the process in order itself: first it redirects a thread parked in a system call (without changing game code and without creating a thread), then it falls back to creating a new thread. Neither path creates a configuration file or touches the game directory, so the game's next normal start is unaffected.

If the game restricts process handles from the moment it is created, live attach fails. In that case, use proxy install instead.

## Conflicts and uninstalling

Anomaly and RE-UE4SS both use `dwmapi.dll` in the game directory, so the two cannot sit there at the same time. When the launcher encounters an unrecognized `dwmapi.dll`, it shows "Conflict" and does not overwrite it. If `dwmapi.dll` and `dwmapi.dll.disabled` both exist, that is treated as a conflict as well.

The launcher has no uninstall button at the moment. To remove Anomaly completely:

1. Exit the game and the launcher.
2. Delete the Anomaly proxy in the game directory: `dwmapi.dll` or `dwmapi.dll.disabled`. Delete only the one you have confirmed was installed by Anomaly, and do not remove same-named files that belong to other tools.
3. Delete `Anomaly\` in the game directory. If you want to keep settings or custom plugins, back up the corresponding files first.
4. To also clear the paths the launcher remembers, delete `AnomalyLauncher.json` in the extracted directory.

## Next steps

- [Quick start](quickstart.md): the first start and bringing up the interface
- [Third-party plugins](third-party-plugins.md): download, update, and uninstall plugins in game
- [Configuration reference](configuration.md): adjust the toggle key, language, and Profile directory
- [NTE Build Profile](nte-profiles.md): why features that depend on the game structure may be unavailable
