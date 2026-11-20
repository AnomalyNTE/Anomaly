# BetterPose Per-Entity Accessory State Design

- Date: 2026-11-20
- Status: Approved for design review; implementation not started
- Scope: `plugins/BetterPose/plugin.cpp` and focused BetterPose tests

## Problem

BetterPose currently owns one global `extra_meshes` collection and one `extra_mesh_owner`. The collection is rebuilt for the currently selected body mesh. On a target switch, `DropExtraMeshes` restores and clears that collection; the next scan builds accessories only for the newly selected target.

The player body can remain parked while another entity is selected: its manual pose/keyframe continues to be maintained, but its accessory collection has already been dropped. The parked body therefore changes pose without its modular accessories being updated. Switching back eventually rebuilds the player's accessories, which explains the observed one-to-two-second recovery.

## Goals

- Keep the accessories of each selected or pose-parked entity synchronized with that entity's body pose.
- Keep the existing single-selected-entity workflow and accessory mapping behavior.
- Preserve capacity checks, object/asset identity checks, original-buffer restoration, and safe handling of component replacement.
- Bound retained accessory memory to entities that are selected or still have parked pose/motion/keyframe state.

## Non-Goals

- Change pose/keyframe authoring, motion retargeting, animation mode, or render snapshot semantics.
- Share one accessory simulation state between different entities.
- Cache every skeletal component in the world or introduce an unbounded accessory cache.
- Change the engine's leader-pose or attachment behavior.

## Proposed Ownership Model

Replace the single global accessory set with an owner-keyed collection:

```text
mesh -> EntityExtraState
```

Each `EntityExtraState` owns the accessory data currently stored in `ExtraMesh`, plus the identity needed to validate that state:

- body mesh, owning actor, and body asset identity;
- scan/build status and accessory candidates for that owner;
- the accessory mappings, bind data, cached original bytes, and independent simulation history;
- per-accessory component class/asset identity and validated buffer metadata.

The collection is accessed and mutated only on the existing game-thread path under the accessory-state mutex. Render callbacks continue to consume snapshots and do not traverse the owner map.

A set is retained while its mesh is selected or has a parked entity pose/motion/keyframe state. It is restored and erased when the entity state is removed, the component is proven stale, or the plugin stops. Target switches do not restore-and-clear unrelated owner sets.

A retained set is only driven while BetterPose owns that entity's pose (selected takeover or parked pose/motion/keyframe). When ownership is handed back to the game's animation, restore the saved accessory buffers and stop writing until the entity is taken over again.

## Data Flow

### Selected Entity

1. The current owner-keyed set is scanned/built if absent or invalidated.
2. The existing pose path computes the final body component-space `PackedTransform` pose.
3. That exact pose is sent to the body write path and to the matching owner's accessory solver.
4. Accessory writes re-read the current array headers and require the current count/capacity to cover the write.

### Parked Entity

1. The existing parked pose/keyframe/motion path computes and writes that entity's final body pose.
2. It passes the same final component-space pose to that entity's own accessory set on the game thread.
3. The accessory solver uses only that set's mappings and simulation state; it must not read the currently selected entity's `bone_names`, `bone_parents`, or body pose as though they belonged to the parked entity.

Where current parked paths do not expose their final packed pose, factor the pose calculation/write boundary so it can return or provide the packed output. Do not reconstruct a parked pose by guessing the active double buffer.

### Scan and Rebuild

- Keep the existing budgeted object scan, but associate each scan's owner mesh/actor/asset with its result.
- A scan completion is committed only if the owner identity still matches the request; late results for a different selection are discarded.
- Empty results retain the existing bounded retry behavior; they do not erase another owner's completed set.
- On a skin/asset change, invalidate only that owner's asset-dependent mapping. Preserve authored pose state as currently defined, restore old accessory bytes only if the old component identity and current capacity still validate, and rebuild that owner's set.

### Removal and Shutdown

- Component class/asset identity mismatch invalidates only the affected accessory entry/set.
- Restoring original bytes is best-effort and capacity-checked; never write saved bytes into a replacement component or a smaller reallocated array.
- Shutdown restores each retained owner's valid accessory entries before releasing its state.

## Concurrency and State Rules

- `EntityExtraState` mutation, scans, and accessory writes remain on the game thread.
- Existing mutex ownership is retained or narrowed to one map lock; never hold it across render callbacks or UI calls.
- A parked entity's pose and accessory set share the same owner key. Switching selection changes the active key, not the ownership of stored sets.
- Invalid or missing body pose data means skip that owner's accessory update for that frame; it must not fall back to another owner's pose.

## Validation

### Focused Tests

Extend the existing accessory/character-switch fixture with two independent owners and disjoint bone layouts. Verify:

- switching active owner does not replace or reuse the other owner's accessory mapping;
- applying a parked body's pose writes only to that owner's accessory buffers;
- each owner's simulation state remains independent across alternating updates;
- a skin change invalidates only the changed owner's mapping;
- a reduced/reallocated buffer capacity rejects writes without modifying adjacent memory;
- removing one owner restores/releases only its own accessories.

Tests must assert observable written transforms and isolation, not merely mock call counts.

### In-Game Acceptance

1. Give the player an accessory and a keyframe pose; select a Character Studio entity.
2. Confirm the parked player's accessory continues following the parked player's keyframe while the other entity is selected.
3. Give the selected entity a distinct accessory/pose; confirm its accessory follows independently.
4. Switch back and forth repeatedly; confirm neither accessory set is exchanged, frozen, or duplicated.
5. Change skin on each entity and repeat; confirm that entity's set rebuilds while the other remains stable.
6. Remove a Character Studio entity; confirm only its set is released and the player can still move and animate.

## Risks and Mitigations

- **Memory growth:** retained sets own saved buffers and simulation state. Bound ownership to selected/parked entity state and erase on entity-state removal or identity loss.
- **Late scan result:** bind scan output to its requested owner identity and discard it if selection/asset changed before completion.
- **Cross-owner pose contamination:** pass an explicit owner and that owner's packed body pose to the accessory update; do not use global active `bone_names`/parents for parked entities.
- **Restore into stale storage:** re-read object identity and array count/capacity at restore time; skip restoration when the original allocation is gone or too small.

## Acceptance Criteria

- The player's accessories continue following its parked keyframe while another entity is selected.
- The selected entity's accessories follow only the selected entity's pose.
- Repeated switching and skin swaps do not mix mappings, freeze accessories permanently, or write beyond current buffer capacity.
- Removing an entity does not affect the player's movement, pose, or accessories.
