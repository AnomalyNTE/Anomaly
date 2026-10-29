# Built-in plugins

The runtime package ships with user-facing built-in plugins. Like third-party plugins, each one is a separate directory package (`manifest.json` + `plugin.dll`), and you can enable, disable, reload and inspect it on the **Plugins** page.

> [!NOTE]
> Features such as coordinates, Entity ESP and WalletCollector depend on a [Profile](nte-profiles.md). When the active Profile is missing a signature or offset, or validation does not pass, the plugin still loads but its features show as unavailable.

> [!NOTE]
> `Nearby Pickup`, `Movement Hold Probe`, `Map Spawn Exporter`, `Teleport Landmarks Probe` and Navmesh Demo are developer test packages: neither the default build nor the released runtime package contains them, and only a runtime package built with `build.cmd testplugins` includes them (see [Building from source](../developer-guide/building.md)). The other plugins ship with the release runtime package.

## Skin Swapper Lite

`Skin Swapper Lite` (`b1ank.skin-changer`) provides character accessories, glider and awakening appearance previews, and custom license plates. After you turn on **Apply plate** on the license plate page, the text follows the supported vehicles you drive afterwards; turning the switch off restores the original plate. **Animated plate** scrolls the text in a loop, and you can adjust the scroll interval. The panel shows the usable characters and the plate width. These changes only affect the local appearance, and awakening previews grant no combat bonuses.

## Coordinate Display

| | |
| --- | --- |
| **ID** | `anomaly.builtin.nte-position` |
| **Purpose** | Read-only display of the current player coordinates, session (World) state and snapshot sampling metrics. |
| **Required services** | `anomaly.ui`; `anomaly.nte.session`, `anomaly.nte.player`, `anomaly.nte.metrics` (all V1, optional) |
| **Requires Profile** | Yes (player / session data depends on validated symbols) |

The plugin only reads data and does not modify game state. Use it to quickly confirm that coordinates and session data are working.

## Teleport

| | |
| --- | --- |
| **ID** | `anomaly.builtin.nte-teleport` |
| **Purpose** | Teleport the player to manually entered or read-out coordinates; supports crosshair-direction teleport, fixed-distance travel along the facing direction, and imported JSON points with category management. |
| **Required services** | `anomaly.ui`, `anomaly.config`; `anomaly.input`, `anomaly.json`, `anomaly.scheduler`, `anomaly.plugin-state`, `anomaly.nte.session/player/player-teleport` (all V1, optional) |
| **Requires Profile** | Yes (player, session and the teleport bridge depend on validated symbols) |

Crosshair teleport advances a preset distance along the current camera direction and clamps the target Z axis above the camera height plus a safety lift, so you do not land underground; forward teleport moves a preset distance along the horizontal facing. You can re-capture both hotkeys in the plugin window. Clicking **Import points** reads a point array from a JSON file path (relative paths resolve against the plugin's private state directory) and supports `color`, `name`, `x/y/z` and an optional `category`; when `category` is missing, the plugin derives the category from prefixes such as `1. Name`. The import result persists with the settings when the plugin is disabled.

## 999 Nights Script

| | |
| --- | --- |
| **ID** | `anomaly.local.nte.night999` |
| **Purpose** | Fully automated monster farming loop for the 999 Nights instance. |
| **Required services** | `anomaly.ui`; `anomaly.core`, `anomaly.nte.session/player/player-teleport/navigation/combat/skills/skill-invocation/actors/entities/pickup`, `anomaly.ue5.names/objects/world/framework`, `anomaly.interop.signature` (all V1, optional) |
| **Requires Profile** | Yes (session and player snapshots, the teleport bridge, reflection lookup and auto combat all depend on validated symbols) |

**This script is for the Red Dragon Castle map in 999 Nights**. The window has two tabs, **Start** and **Settings**.

**Start** page: the **Loop count** input box (**0 = infinite loop**), the **Start script / Stop script** buttons, and the live status line below them (`Round N/M · current step`).

**Settings** page: three teleport points (camp, combat point 1, combat point 2), each with three coordinate rows (X / Y / Z) that you can edit directly, or you can click **Read current position as camp** / **Read current position as combat point 1** / **Read current position as combat point 2** to fill in the player's current coordinates (visible in the same frame); below that come the preload delay, and the auto combat switches and parameters.

A loop always runs six steps: teleport to camp → refresh monsters → teleport to combat point 1 → auto combat → teleport to combat point 2 → auto combat, then back to camp for the next round (repeats the configured number of times, **0 = unlimited**; every teleport waits 2 seconds to settle after landing).

The auto combat end conditions (hitting any one of them ends it and moves on to the next round):

- **First-contact timeout** (default 15 seconds, adjustable): no monster was ever encountered after combat started, so the point is judged empty;
- No monster found for 5 consecutive seconds within the **search radius** (provided by the shared module; the timer only starts after a monster has been seen) — the radius is the "cleared" definition for this point, so it has to cover the whole combat point;
- The basic attack or skill invocation failed (the target does not take damage).

The check does not use damage readings: when the damage stream is unreadable (stale offsets or mismatched participant handles), "how long since the last damage" becomes true while the monsters are still alive, and the script teleports away from a point that is not finished. Damage is only used inside the module to switch targets; it does not take part in deciding whether this point is complete.

## NTE Damage Dashboard

| | |
| --- | --- |
| **ID** | `anomaly.local.damage-dashboard` |
| **Purpose** | Show the combat damage, hits and skill distribution dealt by the player character in real time. |
| **Required services** | `anomaly.ui`; `anomaly.core`, `anomaly.diagnostics`, `anomaly.notifications`, `anomaly.nte.combat`, `anomaly.nte.session`, `anomaly.json`, `anomaly.config` (all V1, optional) |
| **Requires Profile** | Yes (combat snapshots and damage events depend on the validated NTE Combat service) |

The dashboard shows total damage for the run, hit count, highest single hit and critical rate, and also provides health / shield, party damage distribution, skill damage grouped by character, damage-taken sources, timed and permanent buffs, and recent hit records. Skills with the same name are merged, and switching between the four characters does not interrupt the accumulated data.

The plugin only counts damage dealt by the character the player currently controls. The window width is draggable and the height fits the content automatically; combat statistics come from the read-only `anomaly.nte.combat` service and do not modify character attributes or game state.

## Quick Ultimate

| | |
| --- | --- |
| **ID** | `anomaly.builtin.nte-quick-ultimate` |
| **Purpose** | While you hold `Alt` plus a number key `1`, `2`, `3` or `4`, switch to the matching character slot automatically and keep sending `Q`; releasing the number key or `Alt` stops it. |
| **Required services** | `anomaly.input`, `anomaly.ui`, `anomaly.localization` (all V1) |
| **Requires Profile** | No |

The plugin sends keys only while the current game's `UnrealWindow` is in the foreground, and it simulates a plain number-key press and release so the switch does not carry the `Alt` modifier state. It starts sending `Q` as soon as the number key is released: each press lasts `20ms`, then it waits `80ms` after the release and keeps looping while the keys are held. After you release the current number key or `Alt`, the plugin waits for the current `Q` release before cleaning up the key state. When several number keys are held at once, the last pressed number wins.

The plugin only confirms that keys were sent; it does not confirm whether the game accepted the switch or the ultimate skill. Keep the game's default character slot keys and ultimate skill key as `1-4` and `Q`.

## Nearby Pickup

| | |
| --- | --- |
| **ID** | `anomaly.local.nte-pickup-demo` |
| **Purpose** | Pick up `PropBox_`, `InteractBox_` and random item Actors within the pickup radius with one click, and show the host's confirmation state. |
| **Required services** | `anomaly.ui`, `anomaly.localization`, `anomaly.nte.pickup` (V1) |
| **Requires Profile** | Yes; the pickup reflection ABI and the object/name/player/entity layouts have to pass `nte-pickup-layout-v1`. |

The `nearby`, `triggered`, `confirmed`, `checking`, `unconfirmed` and `skipped` values in the interface all come from the framework ABI. `triggered` only means the interaction call finished; the confirmation window lasts at most 2 seconds, and after it expires the plugin keeps `OK` and shows `unconfirmed` separately instead of marking the whole request as failed. The button stays disabled during confirmation; the framework confirms quickly through later entity caches or `bInteractFinish` changes, and only performs one final interactability check at the deadline.

## Entity ESP

| | |
| --- | --- |
| **ID** | `anomaly.builtin.entity-esp` |
| **Purpose** | Draw world-space bounding boxes and labels for entities with the UE native AHUD. |
| **Required services** | `anomaly.config`, `anomaly.ui`, `anomaly.ue5.ahud`; `anomaly.core`, `anomaly.nte.entities`, `anomaly.ue5.names` (all optional) |
| **Requires Profile** | Yes (entity snapshots, name resolution and AHUD drawing depend on validated symbols) |

The plugin window and settings menu keep using ImGui; entity bounding boxes and labels are drawn by the AHUD on the Game thread and stay visible even when the management menu is collapsed. Settings persist through `anomaly.config`.

## Pink Paw Heist ESP

| | |
| --- | --- |
| **ID** | `anomaly.builtin.pink-paw-heist-esp` |
| **Purpose** | Show the loot and extraction points in Pink Paw Heist, with loot filterable by minimum value. |
| **Required services** | `anomaly.ui`, `anomaly.config`, `anomaly.interop.signature`, `anomaly.ue5.framework`, `anomaly.ue5.ahud`, `anomaly.ue5.names`; `anomaly.websocket`, `anomaly.nte.session/player/player-teleport/entities/actors`, `anomaly.font`, `anomaly.texture` (all V1, optional) |
| **Requires Profile** | Yes; drawing loot and extraction points requires a validated `ue5.ahud`, and the generic session, player, entity and Actor services are also gated by the Profile; the Pink Paw specific pickup signatures and layouts ship with the plugin |

The plugin window and settings menu keep using ImGui; item borders, labels and extraction points are now drawn by the UE native AHUD and stay visible even when the management menu is collapsed. The window still lists item names, values, coordinates and extraction point states. The RobBank pickable check and the native pickup call exist only inside the plugin; the host provides signature scanning, Game callbacks, AHUD drawing, name resolution, raw memory reads and the generic NTE services listed above.

In developer mode you can click "Start bank loop" to enter the bank from the open world, or to take over the current bank instance, and pick loot up according to the current minimum-value filter; with "Show access cards" enabled, the loop also tries to pick up each access card once per round. Access cards are secondary targets, and their zero-value objects stay on the map without blocking extraction; loot that reaches the value threshold still has to be confirmed as no longer pickable. Each round first waits for loot to actually be discovered, so an empty list that only contains extraction points right after loading is not treated as complete. Item prices are parsed per unit, and an item with an unrecognized price does not block other targets that are already ready; the panel shows the discovered, ready and filter-matching counts. After a refresh confirms that no targets remain, the plugin walks to an enabled extraction point and sends the game's normal extraction request, and once leaving the instance is confirmed it waits 8 seconds before starting the next round. The panel shows the current phase and the number of completed rounds.

Setting "Bank rounds (0 = unlimited)" to 0 means no limit; with a positive number, the loop stops automatically after confirming that the last round extracted back to the outside map. The round count and the "Skip bank cutscenes" option are saved, and the settings used by a run are fixed once the loop starts. Skipping is on by default; while the loop runs, after confirming that the controller is playing a recognized bank entry sequence, it waits about 2 seconds for the opening, and when the skip button and its parent control are visible, the dialog window is active, and the button is enabled and interactable, it calls the game dialog window's skip flow and lets the game handle the rest of the sequence and the window, requesting it only once per playback. The check still happens when the entry animation comes after the loot is ready; the panel shows the number of skip requests. Other cutscenes still play normally, and the panel reports when the interface is unavailable.

"Stop bank loop" stops all further actions. The loop does not resume automatically after a hot reload or a restart. Unmet entry conditions, unconfirmed pickups, no available extraction point or a wait timeout all stop the loop and show the reason for the specific phase, and failed operations are never counted as complete. When a map change invalidates the pickup context, the plugin re-prepares the loot data even if the previous list had already settled.

On the Pink Paw map the plugin also broadcasts compatible coordinate messages, full loot snapshots and loot deltas through the runtime's local `anomaly.websocket` service, and sends a clear event when leaving the Pink Paw map. WebSocket live positioning in the plugin settings is on by default, listens on `ws://127.0.0.1:14514` by default, and the port is editable in the window. Loot filtering is done by the map front end and is not affected by the value or pickable filters of the ESP menu.

## Custom UID

| | |
| --- | --- |
| **ID** | `anomaly.local.nte.fake-uid` |
| **Purpose** | Change the UID shown in the interface through signature lookup and the object / name services. |
| **Required services** | `anomaly.config`, `anomaly.interop.signature`, `anomaly.scheduler`, `anomaly.ue5.names`, `anomaly.ue5.objects`; `anomaly.ui`, `anomaly.window` (optional) |
| **Requires Profile** | Required for the generic object and name services; the FakeUID specific signatures and layouts ship with the plugin |

> [!NOTE]
> The Custom UID manifest currently declares `builds: ["nte-*"]`, but the signatures and layouts built into the plugin have only been validated on known game versions. The host only provides generic signature scanning, scheduling, object snapshots and name resolution; the plugin checks its own signatures, object layouts and vtables at runtime, shows as unavailable when a check fails, and never writes anyway.

## Camera Tools

| | |
| --- | --- |
| **ID** | `anomaly.local.nte.camera-tools` |
| **Purpose** | Increase draw distance and adjust the view FOV while keeping the character follow camera, or switch to a movable free camera; optionally make the scene stream in around the camera. |
| **Required services** | `anomaly.core`, `anomaly.config`, `anomaly.input`, `anomaly.ui`, `anomaly.localization`, `anomaly.interop.signature`, `anomaly.interop.hook`; `anomaly.nte.session`, `anomaly.nte.player`, `anomaly.nte.player-teleport` |
| **Requires Profile** | No (the camera signatures and layouts ship with the plugin and are validated at load time) |

The extra draw distance defaults to `0`, which means the game default draw distance is used entirely; the plugin imposes no artificial upper bound. The view FOV defaults to `0`, which means **the camera is left completely alone**: the input box then shows the game's current view FOV as read by the plugin (measured at `80` in this game; it stays `0` until a value has been read), and the step buttons start from that number instead of jumping from `0` to the minimum. Entering `0` or clicking **Restore game default** returns to that state and writes the read game FOV back; any other value is clamped to between `15` and `170` degrees and written back to the camera continuously while the plugin runs. The free camera and "stream the scene around the camera" are off by default, and the activation key is `F6`. With that option enabled, the plugin only makes the scene stream in at the local PlayerController's free camera position and rotation while the free camera is active; when it is off, the game's original streaming position is fully preserved.

## WalletCollector

| | |
| --- | --- |
| **ID** | `anomaly.local.nte-interactbox-collector` |
| **Purpose** | Scan the wallet spawn points on the current map, plan a route for the target count, move to each point automatically and confirm through the pickup service that the wallet has been collected. |
| **Required services** | `anomaly.core`, `anomaly.ui`, `anomaly.localization`, `anomaly.nte.player`, `anomaly.nte.pickup`, `anomaly.interop.signature`, `anomaly.ue5.names`, `anomaly.ue5.objects`; `anomaly.nte.session`, `anomaly.nte.navigation`, `anomaly.nte.map-landmarks`, `anomaly.ue5.framework` (all V1, optional) |
| **Requires Profile** | Yes; the player, pickup, pathing and map landmark services come from the active Profile's feature gate, and the wallet point signatures plus the UE5 object / DataTable layouts are also validated while the plugin runs. |

**Wallet target** in the window defaults to `10` and is adjustable between `1` and `500`. After you click **Start Auto Pickup**, the plugin waits for the scan to finish, plans a route starting from the player's current position, moves point by point, waits for the interaction and verifies the pickup result; when pathing stalls it tries other approach directions, when a pickup is unconfirmed it retries automatically, and points that still stay unconfirmed count as "skipped". **Stop** stops the current movement and clears the unfinished route.

**Navigation pickup** is used by default. When the map landmark service is available, the plugin uses it to optimize cross-region routes; when the service is unavailable, it carries on with regular pathing.

## Free Fly

| | |
| --- | --- |
| **ID** | `local.nte.free-fly` |
| **Purpose** | Let the character move freely through the air and through walls: hold WASD to fly horizontally, Space to rise and Ctrl to descend; the toggle hotkey can be captured and saved in the panel. |
| **Required services** | `anomaly.ui`, `anomaly.localization`, `anomaly.input`; `anomaly.config`, `anomaly.nte.session`, `anomaly.nte.player`, `anomaly.nte.player-teleport`, `anomaly.nte.player-hold` (all V1, optional) |
| **Requires Profile** | Yes (player, session and the teleport bridge depend on validated symbols) |

While flying, the plugin has exclusive control of the character: regular movement that is suppressed by the freeze is instead driven by the plugin with the same key set. Movement is placed through `anomaly.nte.player-teleport`, which the host executes with `bSweep=false` and without collision checks, so you can pass straight through walls; gravity freeze prefers the hold entry at the end of the `player` service table, with the standalone service as a fallback. When the camera direction is unavailable, `WASD` degrades to movement along the world axes. The toggle hotkey defaults to F6 and can be re-captured and persisted in the plugin window.

Placement is triggered on demand (when there is input, or when the character deviates from the target by more than 15 centimeters), not every frame: host teleports go through ProcessEvent, and calling it every frame visibly drops frames. Disabling, `on_stop` and `on_unload` all `release` the hold, hand gravity back and turn control off, so no persistent change is left behind.

> [!WARNING]
> Turning flight off at a high position above the ground makes the character free-fall in place and can cause fall damage; descend close to the ground before turning it off.

## Jelly

| | |
| --- | --- |
| **ID** | `anomaly.plugin.jelly` |
| **Purpose** | Make the characters, vehicles and other dynamic models in view keep squashing and stretching (approximately preserving volume), and restore every modified component when it stops. |
| **Required services** | `anomaly.core`, `anomaly.interop.signature`, `anomaly.ue5.names`, `anomaly.ue5.objects`, `anomaly.ue5.framework`, `anomaly.nte.actors`, `anomaly.nte.player`, `anomaly.nte.session`, `anomaly.ui`; `anomaly.localization`, `anomaly.config` (all V1, optional) |
| **Requires Profile** | Yes; the call entry and object registry are validated at load time, and reflection properties are compared by name against the active reflection |

**Amplitude** and **Frequency** set how strong and how fast the squash is, **Max models** limits how many models are affected at once, and **Pause between bounces** keeps a still interval between bounces in which the model returns to its original shape. With **Spin models (up axis)** enabled, models rotate around the vertical axis, and the speed is set by **Spin speed** (the pause only affects deformation, spinning continues as usual). **Include physics-driven vehicles** decides whether whole vehicles (cars being driven) are scaled together with their physics bodies; when it is off, these vehicles do not deform at all, and their attachments are restored along with them.

Hidden, static, instanced and multi-part models are skipped: **Last skip** in the window and the lines starting with `jelly:` in `Anomaly\logs\anomaly-runtime.log` explain why each class was rejected.

## UI Buttons

| | |
| --- | --- |
| **ID** | `local.ui-buttons` |
| **Purpose** | List every UI button in the game, classified as clickable / not clickable / hidden with the reason given; identify a button by hovering over it and pressing the hotkey; invoke the button's click event directly instead of simulating the mouse. |
| **Required services** | `anomaly.core`, `anomaly.ui`; `anomaly.input`, `anomaly.nte.ui-buttons` (all V1, optional) |
| **Requires Profile** | Yes; button scanning, occlusion checks and clicking are all provided by `anomaly.nte.ui-buttons`, and the active Profile has to pass `nte-ui-buttons-layout-v1`. |

The plugin is only a debug panel for the button service: it does not scan objects itself and does not call game functions. Clicking **Scan buttons** builds the button catalog, and **Click** runs after the host re-confirms that the button is still clickable. Buttons that are occluded by other interfaces, disabled or locked are not clickable by default; **Force click** skips that check and can put the game into an abnormal state, so use it for debugging only.

Besides ordinary buttons, the catalog also lists tabs / radio buttons (type shown as **Radio**) and list entries (**List entry**), for example the tabs on the left of the adventure handbook and the character list. Selected tabs still show as clickable; locked tabs and locked entries count as not clickable.

**Button pickup**: hover over a button in the game and press F8 (rebindable in the panel), and the top of the panel shows the buttons under the cursor, with the innermost one first when they are nested. **Pick after 3 seconds** covers situations where pressing a hotkey is inconvenient. With **Show UI layers** checked you can see which interfaces each UI layer currently shows, and which interfaces are judged to occlude the layers beneath them.

## Deserted

| | |
| --- | --- |
| **ID** | `anomaly.builtin.nte.deserted` |
| **Purpose** | A single checkbox: when checked, neither crowds nor traffic spawn any more (the streets are empty), and unchecking restores the values from before it was checked. Stationary NPCs are unaffected. |
| **Required services** | `anomaly.ui`; `anomaly.core`, `anomaly.interop.signature`, `anomaly.ue5.names`, `anomaly.ue5.objects`, `anomaly.input` (all V1, optional) |
| **Requires Profile** | No (locates the crowd/vehicle spawners by its own AOB patterns and depends on the `memory-read`/`memory-write` capability) |

The plugin adjusts two existing sets of knobs directly and does not add or replace the game's spawn logic:

- Crowds: the `HTMassCrowdSpawner` default density tier and its **day/night/clear/rain/snow environment multipliers** on each Density Profile;
- Vehicles: the `MassSpawner` spawn count multiplier and spawn cap, plus the guaranteed online vehicle count of `HTMassVehicleSpawner`.

When checked, it first reads the **current values** of these fields as the initial values and writes them to disk, then pushes them to the minimum; it only writes once when checking and once when unchecking, with no periodic polling or write-back. Disabling or reloading the plugin also restores the initial values, so the world is not left with empty streets. The window hotkey is fixed at F8, the toggle hotkey is changeable and unbound by default; the checked state and the initial values persist in the plugin storage directory.

## Developer-mode debugging plugins

After you enable session developer mode, the plugin catalog also shows `Teleport Landmarks Probe` (`anomaly.builtin.teleport-landmarks-probe`). It enumerates every teleportable landmark the framework provides, showing `TeleportID`, owning world, world coordinates, floor, type and override destination coordinates for each entry, and it lets you submit a `Normal` or `SellingIndulgences` teleport from the options. The plugin only consumes `anomaly.nte.map-landmarks`: it stores no signatures or offsets and does not scan objects, parse DataTables or call UE `ProcessEvent` itself; after developer mode is turned off it no longer appears in the installed plugin view and does not execute teleport requests.

### Movement Hold Probe

| | |
| --- | --- |
| **ID** | `anomaly.local.nte.movement-hold-probe` |
| **Purpose** | Toggle the host's character hold (`anomaly.nte.player-hold`) with one button and show the gravity scale, velocity and movement mode live, so the hold behaviour of the teleport arrival window can be verified on a real client. |
| **Required services** | `anomaly.core`, `anomaly.nte.player-hold` (optional), `anomaly.nte.player` (optional) |
| **Requires Profile** | Yes; `anomaly.nte.player` and `anomaly.nte.player-hold` are provided by the active Profile, and when the service is not published the panel still shows and reports that it is not available yet. |

The panel shows `held` / `refused`, the live gravity scale and velocity, and `mode` (an `EMovementMode` value, `3` being `MOVE_Falling`). **mode=3 is normal** while frozen: the host only holds gravity and velocity and does not change the state the character is in (the game writes its own mode back every frame, so changing it accomplishes nothing). What really decides whether a teleport kills you with fall damage is **where the character is standing at that moment** — the host now teleports first and freezes at the destination, so the fall anchor is at the destination. `refused` means the host rejected the freeze (the reason is written to the host log), and the character then lands as if it had not been frozen.

### Map Spawn Exporter

In developer mode you can also use `Map Spawn Exporter` (`anomaly.builtin.map-spawn-exporter`). After you click **Scan static map**, it enumerates the static DataTables in the UE object registry on the plugin side and parses `TeleportPoint` and Monster spawn rows with reflected coordinate fields, as well as OracleStone and RandomItem rows; it does not walk the UWorld Actor/Entity snapshots. Clicking **Export JSON** atomically writes `map-spawns.json` into the plugin storage directory through host storage, and the file contains the point type, row ID, map name, source table and 3D coordinates. When the candidate DataTable is not loaded or the current Profile does not support it, that category shows as unavailable, and loaded objects are never used to infer points.

### DLL Loader

| | |
| --- | --- |
| **ID** | `anomaly.builtin.dll-loader` |
| **Purpose** | Load the configured native DLL when the plugin is enabled or reloaded, and release the DLL's load reference when the plugin is disabled or reloaded. |
| **Required services** | `anomaly.config`, `anomaly.ui` |
| **Requires Profile** | No |

The default target is `dumper-7.dll`. Put the target DLL into `Anomaly\plugins\DllLoader\` and enable the plugin to load it; you can also enter a package-relative or absolute path in the plugin window. A path change is only kept in memory during the render callback, so reload the plugin from the **Plugins** page afterwards, and the lifecycle saves the settings, releases the old DLL and then loads the new path. Clearing the path and reloading disables DLL loading.


### NTE Vehicle

| | |
| --- | --- |
| **ID** | `anomaly.local.nte-vehicle` |
| **Purpose** | Provide the current driven vehicle snapshot, a vehicle catalog and summoning, speed multiplier, engine torque, wheel friction tuning, and target speed control for loaded non-player vehicles. The default 10x speed, 0.5–10x fine range, 70 km/h baseline and batch target range are all managed by the plugin; Core only validates values, identifies target objects and performs the actual reads and writes. Disabling the plugin requests restoration of the current vehicle's original parameters. |
| **Required services** | `anomaly.ui`, `anomaly.nte.vehicle` v6, `anomaly.nte.player` |
| **Requires Profile** | Yes; Core publishes the vehicle service only when the active Profile's native symbols, layout validation, Game thread and reflection bindings pass. |

The debug key and the fixed SHA-256 digest check are held by the plugin, and Core keeps no key or unlock state. The plugin does not record what you type, the input box uses password masking, and fine speed tuning stays locked when verification fails. Do not write the key into logs, issues, PRs or public screenshots.

### Time Accel

| | |
| --- | --- |
| **ID** | `anomaly.builtin.nte-time-accel` |
| **Purpose** | Set a time dilation multiplier on the local world to speed up the overall game pace (animations, physics, local movement, cinematics, including the player character); supports "boost for N seconds" and "continuous boost", restoring 1x when the time is up or it is stopped. |
| **Required services** | `anomaly.ui`, `anomaly.core` (optional), `anomaly.ue5.names/objects/process-event` (optional), `anomaly.nte.player` (optional), `anomaly.input` (optional), `anomaly.storage` (optional), `anomaly.scheduler` (optional) |
| **Requires Profile** | No (writes world settings through `anomaly.core::write_memory`; depends on the `memory-read`/`memory-write` capability) |

The plugin is only available in **developer mode** (`audience` is `developer`, and it also checks the developer mode switch at runtime). The multiplier, duration and the three function keys (boost once / continuous boost / restore now) are all adjustable in the window and persist as soon as they change; the three function keys are unbound by default. Time dilation is a local world setting, so server-authoritative results (damage, skill cooldowns and so on) do not get faster.

## autofish

`autofish` (`b1ank.autofish`) supports automatic casting at ordinary fishing spots, reeling in after a bite, following the green bar while reeling, and continuous fishing. Open the fishing interface and aim at the water first; after changing fishing spots, cast once manually, then turn on **Enable autofish**.

**Auto-buy bait when empty** lets you pick the bait type and the amount per purchase, and shows the unit price, total cost and stock. When the currently equipped bait runs out, it equips existing stock of the selected bait first and only buys when there is no stock, waiting for the purchase to arrive before continuing. This feature spends in-game currency, and it pauses when the balance is insufficient or a request does not complete, to avoid duplicate purchases.

**Auto sell fish** shows the storage count and the locked count, and once the adjustable threshold is reached it calls the game's one-click fish selling flow and follows its retention rules. Selling only happens between fishing sessions, and fishing continues after the stock decreases. The configuration and the cast point are saved by a background task. The fish-catch display is sped up; the plugin does not offer experimental features such as instant bites or skipping the reeling minigame.

## Managing plugins

On the **Plugins** page you can:

- Search / filter / sort installed plugins.
- Enable, disable or reload a single plugin.
- Inspect plugin state, failure reasons and callback timings.
- Control plugin windows with **Open / Hide**; **Reload all** explicitly triggers a full reload.

To download and install third-party plugins see [Third-party plugins](third-party-plugins.md); to write your own plugin see [Plugin development](../developer-guide/plugin-development.md).
