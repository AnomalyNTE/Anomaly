# Mods

> 🌐 English · [简体中文](../../user-guide/mods.md)

The launcher's **Mods** page manages pak mods inside the game folder: list, enable/disable, uninstall, rename, import a zip, and flip the switch that lets unsigned mods load at all.

## Where it is

Top mode bar of the launcher: **Proxy install / Live attach / Mods**. No game process is required.

The page shows the directory in use:

```
<game>\Client\WindowsNoEditor\HT\Content\Paks\~mods
```

> The path follows the selected client (CN / Global) and the game install location, and falls back to the default `~mods` when nothing matches.

## What it does

| Action | Notes |
| --- | --- |
| List | One row per mod: display name · on/off · file count · size · author · version |
| Enable / disable | Renames the folder or the loose files (adds/removes `.disabled`) |
| Uninstall | Deletes the mod's files or folder |
| Rename | Inline dialog, renames on disk |
| Import zip | Extracts the archive and copies `.pak/.utoc/.ucas/mod.json` into `~mods\<archive name>\` |
| Search | Matches display name, folder name and author |
| Filter | All / enabled / disabled |
| Source | Opens the link from `mod.json` in the browser |

**Every change needs a game restart**: pak containers are mounted at start-up and cannot be unmounted.

## Layout

```
Paks\~mods\
  Mint CSU Prodigy Black\        <- one folder per mod (recommended)
    mod.json
    xxx_P.pak
    xxx_P.utoc
    xxx_P.ucas
  SomeOldMod_P.pak               <- loose files work too, grouped by base name
  SomeOldMod_P.utoc
  SomeOldMod_P.ucas
```

- Extensions: `.pak`, `.utoc`, `.ucas` (IoStore mods ship all three).
- A `.disabled` suffix on the file or folder marks a mod as off.
- `~mods` mounts after the game's own `pakchunk*` files, which is why overrides placed here actually win.

Optional `mod.json`:

```json
{
  "name": "Display name",
  "version": "1.0",
  "author": "Author",
  "modLink": "https://gamebanana.com/mods/xxxxx"
}
```

## Enabling mod loading (signature bypass)

The game refuses to mount pak files without a signature. The **Enable mod loading** switch makes the core turn that check into "always valid" before the engine mounts its paks.

It writes:

```json
{
  "schemaVersion": 1,
  "enabled": true,
  "verifiedFor": "1.4.18",
  "module": "HTGame.exe",
  "pattern": "48 8D ?? ?? ?? ?? ?? E9 ... E9",
  "jumpOffset": 71
}
```

| Field | Meaning |
| --- | --- |
| `enabled` | Master switch, off by default. |
| `pattern` | AOB that locates the check. **Game updates invalidate it.** |
| `jumpOffset` | Offset from the match to the jump instruction (`0x47` on the tested build). |
| `verifiedFor` | Game version the pattern was verified against (informational). |
| `module` | Target module, normally `HTGame.exe`. |

The file lives with the runtime that actually runs:

- **Proxy mode**: `<game>\...\Binaries\Win64\Anomaly\config\pak-signature-bypass.json`
- **Live attach**: `<launcher>\Anomaly\config\pak-signature-bypass.json`

The switch edits whichever exists (both are updated when both exist).

### After a game update

Symptom: no mod loads and `Anomaly\logs\pak-signature-bypass.log` contains

```
pattern not found within the wait window
```

Fix: obtain the pattern for the new build (community config or a read-only scan), update `verifiedFor`, save, and restart the game.

### Limits

- A **restart is mandatory** — mods cannot be toggled while the game runs.
- **Proxy mode only**: live attach happens after the engine has mounted its paks, so mods do not load in that session.
- The bypass modifies the game's runtime checks, which the game's anti-cheat may treat as tampering.
- Logs: `Anomaly\logs\pak-signature-bypass.log` and `Anomaly\logs\mod-operations.log`.

## Troubleshooting

| Symptom | Check first |
| --- | --- |
| Mod does not load | Started through the proxy? `enabled` true? `hooked` line in the bypass log? |
| Empty list | Directory path, file extensions, whether the game holds the containers open |
| Disable button does nothing | Close the game first (files are locked while it runs) |
| Import fails | Valid zip? `tar.exe` available (ships with Windows 10+)? |

## Browsing and installing online

The fourth launcher tab, **Browse**, talks to GameBanana's Neverness To Everness section (game id `23012`).

| Action | Notes |
| --- | --- |
| Search | Keyword + **Search**. Use **English** keywords (`loading`, `hair`, `outfit`): GameBanana barely filters CJK queries and returns unrelated mods |
| Empty query | Browses the newest mods of the game |
| Load more | Appends one page at a time (the service forces 15 per page) |
| Sort | Name / downloads / updated / size; click the same button again to flip the order |
| Thumbnail | 120x68 preview in the row; hover shows the 530px image (downloaded on first hover, cached after) |
| Page | Opens the mod's GameBanana page in the browser |
| Installed | Mods installed by this launcher show "installed" and the button becomes "reinstall" |

Install flow: resolve the file (`ProfilePage`, falling back to `/Mod/<id>/Files` when the page hides files) -> download -> detect the archive by header (zip via `tar.exe`, 7z/rar via the local 7-Zip) -> extract -> **copy the whole tree** into `~mods\<name>\` -> write the cover as `icon.png` -> write `.aurora-source.json`.

Network: the system proxy is preferred (measured ~5 MB/s through the proxy vs ~14 KB/s direct on the reference machine), direct is the fallback, and a failed download retries through `curl.exe`. Reasons land in `Anomaly\logs\launcher-update.log`.

## Which mods actually work

| Kind | Shape | Works here? |
| --- | --- | --- |
| Asset replacement | `.pak/.utoc/.ucas` triplet | ✅ mounted by the engine once mod loading is enabled |
| Asset + companion files | triplet plus image/script folders (e.g. loading screens: pak + 45 jpgs) | ⚠️ the pak mounts, but the companion assets need their tool (NTEUiSwap Plus) to be visible |
| Aurora scripts / modules | only `.lua`, `.dll` or a module manifest | ❌ needs the Aurora runtime |

The installer checks for a pak triplet and rejects script-only archives with an explicit message.

## Update checks

- Reads `.aurora-source.json` (same name and schema as Aurora) from each mod folder.
- A change of `file_id` **or** `md5` means a new version; mods without a source record are not tracked (same as Aurora).
- First run adopts the current newest file as the baseline when `mod.json` carries a `modLink`.
- Rows with an update show `↑ new version vX` plus an Update button that replaces the files and keeps `icon.png`.
## Launcher preferences

Stored in `<launcher>\Anomaly\config\launcher-prefs.json`:

| Key | Meaning |
| --- | --- |
| `browseSort` | Download-page ordering (`default` / `new` / `downloads` / `views` / `likes`) |
| `browseCategory` | Selected category id (empty = all) |
| `hideMature` | "Hide NSFW" state |
| `gridView` | Grid view for the mods page |

## What the GameBanana API allows

| Want | Reality | What we do |
| --- | --- | --- |
| Sort everything by downloads/views/likes | Only `apiv13/Mod/Index` supports it (`Generic_MostDownloaded` / `MostViewed` / `MostLiked` / `Newest`) | Use that endpoint and refetch page 1 on change |
| Search + sorting | Search only accepts `_sOrder=best_match`; other values return 400 | Search is relevance-ordered; category and full-set sorting are hidden while searching |
| Search + category | The search endpoint ignores category filters | Hide the category dropdown while a query is active |
| Download counts in the list | The feed omits `_nDownloadCount` | Fetch per mod from `/Mod/<id>/Files` (same request as the file size) |
| Category filtering | `_aFilters[Generic_Category]` takes a numeric id | Take the id from the record's category URL (`…/cats/43029`) |
| NSFW filtering | NTE records only populate `_bHasContentRatings` | Local filter; also honours `_bHasNsfwContent`, `_bIsNsfw` and an "nsfw" category name |

## Known limits

- While the game runs, enable/disable/uninstall/rename/import/install are blocked: the engine holds `.ucas`/`.utoc` open. The UI greys those buttons and explains why.
- Every change needs a game restart — paks are mounted at start-up only.
- Lua script mods do nothing: this framework has no Lua runtime. Archives containing `.lua`, or pages mentioning Lua/UE4SS/NTEUiSwap, get a `needsLua` flag and the mods page shows a banner.
- Aurora modules (dll/packages) are unusable: they need Aurora's own module loader.

## RE-UE4SS experiment (recorded outcome)

A plugin loaded `ue4ss\UE4SS.dll` at game-ready (official experimental build; `MinorVersion=6` recognises UE 5.6, Lua mods ran — `NTENoTraffic` was verified in game). However, **as long as UE4SS lives in the process, the framework hotkeys become unreliable**, and disabling most hooks and the built-in key mods did not help. The feature is therefore off, and the files were moved out of the game directory.