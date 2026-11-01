# NpcSummon

Lists the actor classes that are actually running in the current world, lets the user filter
them by keyword, spawns the selected class in front of the player and clears the actors the
plugin spawned.

## How it works

1. Signature-resolve `GObjects` / `GWorld` / `ProcessEvent` through
   `anomaly.interop.signature`, then walk the object graph down to the local pawn.
2. Read the actor snapshot through `anomaly.nte.actors` and group live actors by class,
   classifying each class as NPC / vehicle / other from its root component and name.
   Classes without a rendered root (volumes, triggers, `SilentCheckComponent` shells) are
   kept out of the spawn list.
3. Spawn with `GameplayStatics.BeginDeferredActorSpawnFromClass` +
   `FinishSpawningActor`, passing parameters by name (offsets and sizes are read from the
   reflected `UFunction`, never hardcoded per call).
4. Ground the result against the spawned actor's own bounds, because these NPCs use
   `CapsuleComponent` roots whose origin sits at the capsule centre, not at the feet.

`ProcessEvent` is only taken when the full profile signature hits; the generic-prologue
fallback patterns are logged but never called, and a failed resolve disables spawning
instead of calling an unverified address.

## Behaviour notes

- Spawning is explicit: the user picks a class and presses the button. The plugin never
  spawns anything on its own.
- Only actors recorded in the plugin's own spawn list are destroyed by "clear"; world actors
  are never touched.
- The spawn list, whitelist and record file live under `Anomaly\` next to the runtime logs
  (`npc-summon-whitelist.txt`, `npc-summon-records.txt`, `npc-summon-classes.txt`).

## Localisation

`locales/zh-CN.json` carries the Chinese strings. UI text inside `plugin.cpp` is Chinese by
default; the log lines use the same language so runtime logs stay readable next to the
in-game windows.
