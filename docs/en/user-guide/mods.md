# Mods

> 🌐 English · [简体中文](../../user-guide/mods.md)

The launcher's **Mods** page manages pak mods inside the game folder: list, enable/disable, uninstall, rename, import an archive, and flip the switch that lets unsigned mods load at all.

## Where it is

Top mode bar of the launcher: **Launch / Mods / Settings**. Open **Mods** and switch between **Manage / Download**. No game process is required.

The page shows the directory in use:

```
<game>\Client\WindowsNoEditor\HT\Content\Paks\~mods
```

> The path follows the selected client (CN / Global) and the game install location, and falls back to the default `~mods` when nothing matches.

## What it does

| Action | Notes |
| --- | --- |
| Grid (default) | The view switch beside the list, with the grid first and selected: categories first, each card showing the cover of the category's first mod and its mod count; a click opens that category, which lists its skins as cards of their own (each showing that skin's first cover and mod count) when it has more than one, and a further click opens one skin's mods. Above the cards is the path: **All** returns to the outermost level and every named level in the path is clickable too, so no level needs to be left one step at a time. Every mod appears once, and the skins it also covers are part of its card metadata. Every level uses cards of the same size, covers are shown whole, several fit a row, and each card has a surface of its own; a mod card puts the enable switch, the batch box and, when the mod has them, **Source** and **Update** on its first row, and only **Settings** and **Uninstall** under them |
| List | The same data as rows: switch, cover, name, file count, size, author and version, then the same commands on the right: Update (with a new version), Settings, Uninstall and Source (with a link), and the batch box last. A command a mod does not have leaves its column empty, so the box lines up down the list |
| Pagination | **Off by default**: both views list every mod at once and only the list area scrolls. Turn it on under **Settings > Mods page** and set the page size; the pager is a floating glass capsule centred at the bottom of the list |
| Enable / disable | Hot mode saves a selection for runtime mount/unmount; compatibility mode adds/removes `.disabled` on container files while the game is closed |
| Enable switch | Enabling a mod disables conflicting model/resource replacements and synchronizes with the in-game Mod Manager |
| Uninstall | Deletes the mod's files or folder |
| Settings | One dialog holds everything about a mod: display name, its file or folder name on disk, group, notes and the GameBanana link. Saving renames first and then writes the notes |
| Import archive | Extracts ZIP, 7z or RAR archives and copies `.pak/.utoc/.ucas/mod.json` into `~mods\<archive name>\` |
| Search | Matches display name, folder name and author |
| Filter | All / enabled / disabled tabs; changing the tab or search resets to page 1 |
| Source | Sits on the switch row when `mod.json` carries a link, and opens it in the browser |
| Page / Install | On the download page a row reads name, then the two commands under it -- **Page** and **Install** side by side, with an empty page column for a mod without a link so the install button lines up down the list -- and the row facts after them on the same line, centred vertically against the buttons |

**Whether a change needs a restart depends on runtime loading**: without it, paks are mounted at
start-up only and every change needs a restart. With it, containers can be added, removed and
replaced while the game runs; see [Runtime loading and hot swap](#runtime-loading-and-hot-replacement).

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

## Enabling mod loading

The switch writes `enabled` in `Anomaly\config\pak-mods.json` and defaults to on. Hot loading supports
proxy startup, launch-and-attach and live attach when the active Profile passes the `ue5.pak` symbol,
ABI and layout checks.

- All three entry points and the in-game Mod Manager share `Binaries\Win64\Anomaly\config\pak-mods.json` in the game installation.
- Until that file exists, the old Runtime-local file is a migration fallback. The first save establishes the shared file.
- Launcher and the plugin serialize read/modify/atomic-write operations with the same file mutex, preserving custom `mods` arrays and other fields.
- Invalid configuration causes a save error. The Runtime preserves existing mounts and reports `lastError`.

```json
{ "schemaVersion": 1, "enabled": true, "mode": "hot",
  "modStates": { "OldVehicle/": false, "NewVehicle/": true, "Sound.pak": true } }
```

Directory keys in `modStates` end in `/`; loose containers use their relative `.pak` filename. Choices
filter both directory discovery and explicit manifests. Hot disable leaves mounted filenames intact,
and reenabling an unchanged file mounts it again.
Loose containers retain the full stem: `foo` and `foo_P` are distinct mods.

**Compatibility mode** sets `mode` to `compatibility` and uses the previous proxy startup signature
bypass so the engine loads mods itself. Close the game before changing modes, then start through the
proxy. Compatibility mode installs no hot-loading cache hooks and rejects runtime mount, unmount and
prime commands. Its selection changes require restarting the game. Mode conversion preserves selections
through `.disabled` markers. The mode stays fixed for the game process, including Runtime restarts.

The compatibility switch preserves and enables an existing `pak-signature-bypass.json` recipe; saving
fails if no signature recipe exists. Hot mode does not start this legacy path. Its `pattern` must match uniquely;
`jumpOffset` identifies an E8/E9 branch. The supported legacy `patchBytes: "B0 01 C3"` form hooks a bool
predicate directly. Other raw patch sequences are rejected. `verifiedFor` is descriptive only.

## Runtime loading and hot replacement

The Runtime checks change notifications for `Paks\~mods` every 750 ms by default and scans initially or after changes, skipping `.disabled` files/directories and reparse
entries. Each `.pak` is one container. Default read orders start at **100000** and increase; the engine's
read-order rules determine which duplicate resource wins. Default ids are relative paths without the
extension, such as `Outfit/Shared`, so equal basenames in different folders remain distinct.

An explicit manifest can select the mounted set:

```json
{ "schemaVersion": 1, "enabled": true,
  "mods": [ "MyMod\\MyMod.pak", { "id": "Sound", "pak": "Sound.pak", "order": 100100 } ] }
```

Relative paths resolve against `Paks\~mods`. A `mods` array is authoritative; without one the directory is
scanned. `enabled: false` stops automatic mounting and attempts to unmount containers owned by this Runtime.
Invalid configuration, missing manifest files or duplicate ids/paths preserve the current set. Failed unmounts remain tracked and
block all new mounts in that pass, preventing two conflicting variants with different ids from becoming active together.
Initial loading, file/configuration changes and explicit synchronization copy mounted filenames under the engine's mount-list lock and remove startup
mounts under `~mods` that this Runtime does not own before applying the selection. Unmount uses the
engine's original filename spelling so relative paths cannot leave an old or disabled container mounted.
Stable polls check directory change notifications without rescanning, replanning mounts or taking the engine's
mount-list lock. Catalog snapshots also use notification invalidation. Directory moves, deletion, recreation and
external manifest directories remain watched. Unavailable notifications fall back to scanning, and ordinary mount
or unmount failures still retry automatically. A native mount exception sets `nativeFault: true` and stops background
retries for that file until the file changes or an explicit reload requests another attempt.

```text
anomaly-cli --pid <game PID> pak status
anomaly-cli --pid <game PID> pak performance
anomaly-cli --pid <game PID> pak release "My Mod/MyMod"
anomaly-cli --pid <game PID> pak reload
anomaly-cli --pid <game PID> pak mount "My Mod/MyMod.pak" 100100
anomaly-cli --pid <game PID> pak unmountall
```

- A successful `release` stays `released` until timestamps or sizes change for `.pak/.utoc/.ucas/.sig`.
  `unmountall` also stays released on the next poll. Use an explicit `mount` to remount unchanged files.
- Manual mounts remain subject to subsequent manifest/directory reconciliation. Add external paths to
  the manifest to retain them.
- **Successful unmount does not guarantee closed file handles.** UE can retain outstanding reads and
  open resource handles. Do not force an overwrite while files are busy; wait or exit the game.
- **Do not copy a multi-file container into an active watched location one file at a time.** Disable
  automatic loading or temporarily remove the manifest entry, prepare the complete file set outside
  the watched directory or in a `.disabled` folder, then publish it and restore the configuration.
  Polling is not a multi-file transaction. Equal timestamps and sizes do not detect changed contents.
  Use this full disable/enable workflow for partitioned or optional containers too.

State is published to `Anomaly\state\pak-mods.json` and to the `pakMods` node in `diagnostics-summary.json`.
`mode` identifies the process's loading strategy and `pid` identifies its owner. The explicit
`residentAssetsReloaded: false` field means container switching did not refresh resident assets.
`pak performance` reads counters without triggering scans. While stable and idle, `reconcilePasses`,
`mountedListReads` (list reads during reconciliation) and `catalogMetadataScans` should stay unchanged,
with `retryPending` set to `false`.

### Native calls and diagnostics

Mounting validates the Profile, resolves `FPakPlatformFile`, installs cache verification routing and
configures signature support before calling native `Mount(FPakMountArgs, FIoStatus, FPakListEntry)`.
Pure pak files use `SkipContainerFile`; the presence of `.utoc` selects native IoStore mounting. File
sizes do not determine the container type. The output entry transfers a strong reference that the
Runtime releases after unmount or during shutdown. Every required signature step must succeed.

Before mounting an IoStore mod, the Runtime compares its Blueprint, struct and enum serialization
definitions with the currently mounted game containers, including parents, field order, array dimensions
and nested types. An incompatible definition and its affected dependencies within the candidate container
are retained from the game in a separate compatibility container. Other mod resources can still load.
Retained package names are reported in mount status and logs; this policy has no mod-name or vehicle exceptions.
Unreadable formats, inseparable new dependencies and unsupported optional packages are refused before mounting,
preserving the existing mounted set for that pass. This inspection covers supported Zen package formats;
legacy standalone pak files keep their existing path, and schema compatibility does not guarantee script behavior.

Unmount drains async package loading on the Game thread, then removes the source before its compatibility
container. Partial failures retain ownership for cleanup on a later pass and block new replacement mounts.
Compatibility cache files are addressed by content and verified on reuse; existing files are never overwritten
while the engine may still hold them.

The cache hook samples ownership while holding the engine's cache lock. Existing signature tables use
the original verifier. A confirmed null table goes through native completion, which obtains the read
results and maintains queues/counters. Unreadable state uses the original function. The hook does not
manually transfer request buffers. Shutdown detaches and drains the diagnostic sink; the resident Core
retains cache routing for late native I/O.
Restarting the Runtime in the same process reuses the previously validated Pak binding, since installed
patches change the bytes used by signature scanning.

Signature tables come from `.sig`; **a diagnostic read does not create a missing signature table**.
Mounting no longer runs automatic reads or global index optimization. An explicit `pak prime "<id>"`
probes one owned file and reports its stage. It is a diagnostic, not a repair or proof that all assets
will load. An IoStore-only container can lack data for this pak read path without its mount having failed.

Signature changes affect the process. Disabling automatic loading or stopping the Runtime does not
restore every signature setting; restart the game for full restoration. Missing symbols or incompatible
layouts disable the feature rather than guessing addresses.

### Asset reload limits

This feature manages container mounts. Unmounting does not destroy loaded `UObject` / `UPackage` objects,
rebuild the Asset Registry, refresh materials/audio, or replace resident assets. A `mounted` state does
not mean existing resources have been reloaded. Resident assets, outstanding asynchronous loads and full
replacement behavior still need in-game validation. Restart when a clean resource replacement is needed.

When ModManager refreshes a mesh, it preserves a dynamic material instance only when the slot mapping
and material identity remain continuous. A different material uses the new mesh's defaults, preventing
an old skin's textures from being carried to another model. Detached instances with no other compatible consumers keep their parent
and textures so a failed transaction can restore the previous component bindings.

The independent **Mod Manager** plugin and Launcher classify mods from `.pak` and `.utoc` directory indices.
The classification has two levels: one group per character, and the skins inside it. Zero's base outfit and
swimsuit land in the `female` and `female_swimsuit` skins of `Player_051` (`Player_051/female` and
`Player_051/female_swimsuit` as the index spells them). A mod that covers several skins is filed under the first
of them and carries the others as tags on its cover or row, so it neither makes a category out of the combination
nor shows up twice; the skins of one character can be on at the same time, and their mod counts add up to the
count of the character. Blackbird's base and evolved forms are different skins. Skin mesh ownership takes
priority over companion materials and variant helpers; texture-only mods use their resource directories.
The pendant replacement uses `Pendant_07`; borrowed texture filenames do not change ownership. Other resources use their content directory,
such as `/game/wwiseaudio` for audio. Unreadable indices remain unclassified. A category with a single skin
does not grow an extra level; its mods are listed directly. A hand-written group replaces the automatic
classification and takes that mod out of the two levels.
Grouping alone does not make mods mutually exclusive: non-overlapping outfits can coexist. A mod that covers
several skins conflicts with a mod covering any of them and coexists with the mods of skins it does not touch --
exclusivity always comes from resources and chunk IDs, never from a whole character or skin.
Resource cleanup remains limited to vehicles.
GT3 RS and P1 cover `Vehicle_037`; Tourbillon covers `Vehicle_026`. Mods conflict when they override the same resource
or replace the body/blueprint of the same vehicle. Enabling one disables conflicting variants in the same saved selection.
IoStore checks both paths and chunk IDs: shared resources with equal content hashes and lengths can coexist.
Different or unknown content remains exclusive. Only index metadata is read, not large resource payloads.
Encrypted, missing or unsupported indices are
reported as incomplete; archive and mod folder names are not used to guess the category. Launcher and the plugin share parsing and selection logic.
Known conflicts introduced by manual configuration edits freeze only the conflicting mods at their current mounts;
unrelated mods continue synchronizing and hot replacing. The status file lists affected mods in `conflicts`.
Selecting a single variant resolves the conflict.

**Load conflicting mods anyway** (switch on the top switch row of the mods page, right of Compatibility mode; it writes `forceLoad` in
`config/pak-mods.json`): exclusivity no longer stops two mods from being enabled together, and Runtime no longer
freezes their mount changes because of resource conflicts. The engine's effective read priority selects overlapping
resources. The switch does not change that priority or guarantee that the last selection or mount wins. For example,
a vehicle replacement and a patch for its body textures can load together, but still need the correct resource priority.
The switch does not merge their contents; textures and skeletons from different packages may still be incompatible.
Turning it off restores exclusivity for subsequent enable operations. In hot mode, existing conflicting mounts stay
in place and their changes freeze until a single variant is selected. In compatibility mode, turning it off does not
rename already enabled files; select a variant again before starting the game.
While it is on, hot-mode status lists affected mods under `forcedConflicts` and leaves `conflicts` empty.

#### Mod folders: a mod or a category

Folders under `~mods` are read by what they contain, never by their name:

- a folder that directly holds a `.pak` / `.utoc` is one mod;
- a folder that ships `mod.json`, `anomaly-source.json` or a cover (`icon` / `cover` / `preview` /
  `thumbnail`) is one mod as well, even when its containers sit in `Paks/` below it;
- a folder that only holds folders is a category folder, and every mod inside it is listed on its own.
  `Paks/`, `Content/`, `HT/`, `WindowsNoEditor/`, `LogicMods/`, `Binaries/`, `Data/` and `Config/` are
  container layout folders: they belong to the mod above them and never become a category.

So `~mods/character/ModB/xxx.pak` is the mod `ModB` inside the category `character`,
`~mods/ModB/Paks/xxx.pak` is still the single mod `ModB`, and `~mods/xxx.pak` is a loose mod. Category
folders nest, and a mod's selection key is its own relative path (`character/ModB/`); a selection written
for the category folder (`character/`) still covers every mod below it.

"Organise folders" files each mod under a folder named after its category
(`~mods/Player_051 零/ModB/`) and moves its selection key with it: a key is a path, so a key left behind
would quietly turn off mods back on. Mods already in place stay where they are, and a target that already
exists is skipped instead of merged. It also runs while the game is open: a folder the engine still holds
open is skipped and reported, and one more pass after closing the game finishes it; a mod filed by hand
under some other folder is moved to the folder its category names.

After a manual folder move, each container with no saved selection for its current path inherits the selection
saved for its mod's name: the folder name, or the `.pak` filename of a loose container. Multiple containers in
one mod count as one mod. Current file or directory selections take precedence, and physically disabled files
stay disabled. Inheritance requires a unique mod name across the complete directory tree; namesakes are not
resolved by name. If historical locations disagree, OFF takes precedence so an old ON entry cannot reactivate
the mod. An explicit selection, or an automatic disable due to a conflict, removes uniquely attributable old
keys that no longer reach an existing mod and saves the selection at the current path.

#### Category names

The name a category is shown under comes from a shipped data table,
`Anomaly/assets/mod-groups/mod-groups.json`:

```json
{
  "schemaVersion": 1,
  "groups": { "Player_051": { "zh-CN": "零", "en-US": "Zero" } },
  "skins": { "female": { "zh-CN": "女装", "en-US": "Female" } }
}
```

- A category the table does not mention still gets a readable name: `Player_007` → Character 007 /
  角色 007, `Vehicle_037` → Vehicle 037 / 车辆 037, `Pendant_07` → Pendant 07 / 挂件 07. A content
  directory such as `/game/wwiseaudio` keeps its own spelling.
- A name is shown in the language it is written in: an entry with no name for the current UI language
  falls back to the generated category name instead of showing the other language's name, so an
  English page never reads a Chinese character name or organises mods into a Chinese folder. A Chinese
  page still reads a Latin vehicle name such as `Old Times`.
- `Anomaly/config/mod-groups.json` is read after it and overrides or adds an entry; entries it does not
  mention are kept, so a new character or car needs one line there and no package update.
- Vehicles use the same mechanism as characters: the `Vehicle_*` mapping names both the display value and
  the category folder (`~mods/Vehicle_037 车辆 037/`).
- The identifier itself is not localized: renaming a category changes neither classification nor
  exclusivity.
- The shipped table comes from the game's own data: a row of `DT_Character` named `1NNN` is the
  container folder `Player_0NN`, its `ItemName` is the character's name and `MeltPackID` says which body
  the row is -- 零 exists as both the male and the female character, so they read as 「零（男）」 and
  「零（女）」. A row of `DT_VehicleItemData` named `vehicleNNN` is `Vehicle_0NN` and carries the car's name.
  A name this client carries in Chinese is recorded as `zh-CN` only; a vehicle whose name is Latin
  letters gets an `en-US` entry as well.
- A game update is one export away: the developer test plugin `ModNameExport` (not built or installed by
  default; `build.cmd testplugins` builds it and the `TestPlugins` component installs it) reads both tables
  on launch and writes nothing but
  `Anomaly/state/plugins/anomaly.mod-name-export/mod-group-names.json`, which is read after the shipped
  table -- no package edit, and the next mods-page refresh shows the new names.
- The in-game Mod Manager uses the same names: its package ships
  `Anomaly/plugins/ModManager/assets/mod-groups.json` (Chinese names only, generated from the same data;
  a plugin can only read the files the host copied into its package). It shows the character and vehicle
  names while the UI is Chinese and the generated `Character 051` / `Vehicle 037` while it is English;
  a name never changes classification or exclusivity.

The alternative wheel finishes in the example P1 archive should be imported individually. All containers inside one mod
folder form one unit; the manager does not choose between variants packed into the same folder.

1. Open Mod Manager, confirm hot mode, and use the mutually exclusive switches shared with Launcher. The
   window no longer pages: it lists every mod under its category at once, and only the list scrolls -- the
   mode, the status, **Reload world** and the search box stay at the top. The search matches the mod name,
   the category identifier, the Chinese category name and the skins, so a Chinese and an English input find
   the same row.
2. After container changes, the plugin automatically attempts to release resources from all variants of the vehicle.
   Each vehicle group keeps one **Release resources** button for manual retries.
3. If instances remain, exit the vehicle and use the group's single **Force cleanup** button. The plugin validates object serials,
   actor types and owning world, protects the current occupied vehicle, and calls the game's
   `HTWheeledVehicleDrivable.SafeDestroyVehicle` to retire its Mass vehicle entity too.
   This button only clears vehicles. If a previous mod switch was blocked by those vehicles, completion resumes one resource release.
4. Resource release invalidates the target vehicle's old physics template lookup keys before dropping shared simulation parameters,
   preventing later summons from accessing classes collected by GC. It then invokes `HTGameUserSettings.ClearCacheUIAndGC`,
   waits for GC, and checks matching actors and original package handles again.
5. Summon again to check the model. **Partial release; still resident** reports remaining old resources, not failure of the
   entire mod switch. The shared `DT_vehicleModuleData` table is not forcibly released and its contents may remain unchanged.
   Newly loaded packages at the same path do not count as retained old packages.

Cleanup uses Profile and reflection-validated layouts and engine calls without changing UObject GC flags.
Invalidated physics template allocations remain owned by the native subsystem until world teardown, preserving raw borrowers in other caches.
Shared asset/UI caches are cleared and may briefly stall or rebuild UI. Container indices do not describe every cross-package
reference; a scene reload or game restart may still be necessary. The GT3/P1 visual switch still requires live validation.
The plugin reports the mode chosen for the actual process. Compatibility mode disables all hot operations; make changes in
Launcher after closing the game. The new host service requires updating Core and restarting the game; plugin hot reload alone
cannot add this interface to an older Core.

## Troubleshooting

| Symptom | Check first |
| --- | --- |
| Mod has no effect | `available`, `state`, `lastError` in `pak status`, and the engine reason in `pak_mods.mount_failed` |
| Empty list | Directory path, `.disabled` markers, and the manifest's `mods` array |
| Cannot overwrite/disable files | Release status and outstanding reads; exit the game if files remain busy |
| Mounted but resources unchanged | Whether resources were already resident; mounts do not refresh them |
| Read failure | Cache hook availability, Profile compatibility, complete file set; `pak prime` is diagnostic only |
| Archive import fails | Check archive integrity, encryption and available disk space; see the extraction error |

## Browsing and installing online

The fourth launcher tab, **Browse**, talks to GameBanana's Neverness To Everness section (game id `23012`).

| Action | Notes |
| --- | --- |
| Search | Filters loaded mods by name and author as you type |
| Empty query | Browses the newest mods of the game |
| Automatic loading | Opening Browse for the first time requests the first 15 mods |
| Refresh list | Requests page 1 again and replaces the list on success; keeps the existing list while loading or on failure |
| Load more | Appends one page at a time (the service forces 15 per page) |
| Sort | Liquid glass tabs: name / downloads / size / time; click the same tab again to flip the order |
| Thumbnail | 120x68 preview in the row; hover shows the 530px image (downloaded on first hover, cached after) |
| Page | Opens the mod's GameBanana page in the browser |
| Installed | Mods installed by this launcher show "installed" and the button becomes "reinstall" |

Install flow: resolve the file (`ProfilePage`, falling back to `/Mod/<id>/Files` when the page hides files) -> download -> detect ZIP / 7z / RAR / RAR5 by content (bundled decoders; no 7-Zip installation required) -> extract -> **copy the whole tree** into `~mods\<name>\` -> write the cover as `icon.png` -> write `anomaly-source.json`.

Extraction uses a unique temporary directory with automatic cleanup and Unicode paths. Unsafe paths, links, duplicate files and encrypted archives are rejected; extraction is limited to 16 GiB / 20000 entries. The download Cancel button also cancels extraction.

Network: the system proxy is preferred (measured ~5 MB/s through the proxy vs ~14 KB/s direct on the reference machine), direct is the fallback, and a failed download retries through `curl.exe`. Reasons land in `Anomaly\logs\launcher-update.log`.

## Which mods actually work

| Kind | Shape | Works here? |
| --- | --- | --- |
| Asset replacement | `.pak/.utoc/.ucas` triplet | ✅ mounted by the engine once mod loading is enabled |
| Asset + companion files | triplet plus image/script folders (e.g. loading screens: pak + 45 jpgs) | ⚠️ the pak mounts, but the companion assets need their tool (NTEUiSwap Plus) to be visible |

The installer checks for pak/utoc/ucas asset files and reports missing assets when none are found.

## Update checks

- Reads only `anomaly-source.json` from each mod folder: `schemaVersion: 1`, `provider: "gamebanana"`, `modId`, `fileId`, `fileName`, `fileMd5`, `author`, and `displayName`. Both IDs are strings.
- A change of `fileId` **or** `fileMd5` means a new version; mods without a source record are not tracked.
- First run adopts the current newest file as the baseline when `mod.json` carries a `modLink`.
- Rows with an update show `↑ new version vX` plus an Update button that replaces the files and keeps `icon.png`.
## Launcher preferences

Every launcher interface choice is stored in its own configuration, `<launcher>\AnomalyLauncher.json` (the old `launcher-prefs.json` is gone). Changes are written as they happen, so closing and reopening the window keeps them:

| Key | Meaning |
| --- | --- |
| `theme` | Palette the launcher wears (default `naiwa`) |
| `language` | Launcher language: `auto` (follow Windows), `zh-CN`, `en-US` |
| `view.modsGrid` | Mods page layout (`true` = grid, the default; `false` = list) |
| `view.modsPagination` | Whether the mods page paginates (default `false`) |
| `view.modsPageSize` | Mods per page when it does (default `10`) |
| `view.browseSort` | Download-page ordering (`default` / `new` / `downloads` / `views` / `likes`) |
| `view.browseCategory` | Selected category id (empty = all) |
| `view.browseHideMature` | "Hide NSFW" switch state, `true` by default |

## What the GameBanana API allows

| Want | Reality | What we do |
| --- | --- | --- |
| Sort everything by downloads/views/likes | Only `apiv13/Mod/Index` supports it (`Generic_MostDownloaded` / `MostViewed` / `MostLiked` / `Newest`) | Use that endpoint and refetch page 1 on change |
| Keyword filtering + sorting / category | The input filters the loaded list | Keep the current sort and category; no extra search request |
| Download counts in the list | The feed omits `_nDownloadCount` | Fetch per mod from `/Mod/<id>/Files` (same request as the file size) |
| Category filtering | `_aFilters[Generic_Category]` takes a numeric id | Take the id from the record's category URL (`…/cats/43029`) |
| NSFW filtering | NTE records only populate `_bHasContentRatings` | Local filter; also honours `_bHasNsfwContent`, `_bIsNsfw` and an "nsfw" category name |

The launcher's mods page does not paginate by default: both views list every mod at once, the path, switches, search box and filters stay pinned at the top, and only the list area scrolls. The download page scrolls the same way, and reaching the bottom loads the next page -- it keeps loading while the list is still shorter than its viewport. Turning pagination on under **Settings > Mods page** splits the set by the configured page size, with the pager floating at the bottom of the list. Launcher uses a grid of covers or a list of rows, both carrying the same commands; Group / notes also supports ungrouped mods and loose files.

## Known limits

- Import and download remain available while the game runs. Hot mode can switch mods; disabled mods can be updated, renamed or uninstalled once the engine releases their files. Compatibility mode cannot switch live; new imports stay disabled until enabled after exiting the game.
- Import and update keep the target disabled until all container files have been copied. Failed operations leave it disabled.
- Without runtime loading, every change needs a game restart — paks are mounted at start-up only.
  With it, containers can be added, released and replaced at runtime
  (see [Runtime loading and hot swap](#runtime-loading-and-hot-replacement)).
- The launcher mounts asset containers and does not execute Lua scripts shipped with a mod.

## RE-UE4SS experiment (recorded outcome)

A plugin loaded `ue4ss\UE4SS.dll` at game-ready (official experimental build; `MinorVersion=6` recognises UE 5.6, Lua mods ran — `NTENoTraffic` was verified in game). However, **as long as UE4SS lives in the process, the framework hotkeys become unreliable**, and disabling most hooks and the built-in key mods did not help. The feature is therefore off, and the files were moved out of the game directory.
