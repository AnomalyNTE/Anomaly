# NTE character combat ABI plan

## Document status

- Status: v1 is implemented; see [`docs/api-reference/nte-services.md`](../api-reference/nte-services.md) for the authoritative contract.
- Research baseline: `C:\Dumper-7\5.6.1-0+UE5-HT-1.3\CppSDK`.
- Goal: give plugins a character combat snapshot, damage statistics, a skill catalog and controlled skill invocation without exposing UE object pointers.
- First-release principle: publish only capabilities jointly validated by the active Profile, reflection shape, object generation and the Game thread; when any gate fails, the whole service degrades to unavailable.

## Scope

The first release is split into three independent services:

1. `anomaly.nte.combat`: the current character combat snapshot, the character-native damage event stream and conditional aggregation.
2. `anomaly.nte.skills`: a paged snapshot of the skills granted to the current character, their activity state and verifiable cooldown information.
3. `anomaly.nte.skill-invocation`: requests skill activation using a current-generation skill handle returned by `anomaly.nte.skills`.

The first release does not include:

- Exposing any UObject address, `UFunction*`, `FGameplayAbilitySpec*` or GAS/HTGame native structure across the DLL boundary.
- Invocation by arbitrary function name, object path, bare ability class or argument buffer.
- Calling `HTServerTryActivateAbilityWithData` directly, and parameterized casting with a specified position, rotation, target, section or prediction key.
- Modifying health, damage, faction, cooldown, skill grants, buffs or GameplayEffects.
- Declaring client damage floating-text data as a server-authoritative combat log.

## SDK evidence

The following comes from Dumper output and serves only as offline evidence for candidate structures and reflection contracts. The implementation must still be validated at runtime by the active Profile.

### Damage entry

| Candidate | SDK evidence | Conclusion |
| --- | --- | --- |
| Damage entry | Native broadcast of `AHTAbilityCharacter::CharacterOnDamaged` | The exact broadcast template has a single call site and bypasses floating text and global event dispatch |
| Damage value | `FHTDamageEvent::Damage` at offset `0x10` | Written to `final_damage` as the actual damage |
| Participants | The `DamagedCharacter` and `InstigatorCharacter` arguments of the broadcast | Converted into an opaque handle made of the object index, serial and current registry generation |
| Source | `FHTDamageEvent::DamageGEDef` at offset `0x138` | Resolves the Gameplay Effect object path and maps it to a source id of the current combat generation |
| Display metadata | The broadcast carries no floating-text classification | `display_damage`, `basic_damage`, critical, head hit and position fields are not guessed |

Captured events carry the `CHARACTER_EVENT` flag, meaning the value and the source come from the character damage broadcast; `CLIENT_PRESENTED` is reserved for a possible standalone display event and is not used on this capture path.

### Character combat state

`AHTAbilityCharacter` exposes the following reflection candidates:

- `K2_GetAbilitySystemComponent()`: parameter size `0x08`, returns `UHTAbilitySystemComponent*`.
- `GetHP()`, `GetHPMax(bool)`, `GetIsDead()`, `GetAttackTarget()`.
- `UHTAttributeComponent` provides read-only functions such as `GetHPCurrent()` and `GetShieldHealth()`.

The first release captures only the `hp`, `max_hp`, `shield`, dead state and attack target of the current player character. Attack power, tenacity, unbalance and elemental damage bonus/resistance are visible in `UHTAttributeComponent`, but the field set is large and the meaning depends on gameplay, so they are deferred to a separate attribute page ABI to avoid freezing a build-specific attribute table into core combat v1.

### Skill catalog and invocation

| Candidate | SDK evidence | Conclusion |
| --- | --- | --- |
| ASC lookup | `AHTAbilityCharacter::K2_GetAbilitySystemComponent()` | Obtained through a validated ProcessEvent call without hardcoding a character member offset |
| Granted skill container | `UAbilitySystemComponent::ActivatableAbilities` at `0x408` in the current SDK | Used only by the Profile/validator and kept out of the public ABI |
| Container items | `FGameplayAbilitySpecContainer::Items` at `0x108` | Enumerated as a bounded read-only `TArray` snapshot |
| spec | `FGameplayAbilitySpec` is `0xF0` bytes; `Handle=0x0C`, `Ability=0x10`, `Level=0x18`, `InputID=0x1C`, `ActiveCount=0x28`, state bits `0x29` | Produces a host-owned skill snapshot; every offset must exist in the Profile and be validated by the reflection shape |
| Skill class | `FGameplayAbilitySpec::Ability` is the ability CDO; its `object.class` points at the actual `UClass` | Returns an object handle and the full UTF-8 path; on invocation the host resolves it again and confirms it is still the same granted spec |
| Cooldown | `UHTGameplayAbility::CooldownGameplayEffectClass` at `0x1B8`; the SDK parameter struct of `UHTAbilitySystemComponent::GetActiveEffectTimeRemainingAndDuration` is `0x10` | Queries remaining time and duration through a validated GameplayEffect class; when the shape does not validate, only that skill is marked `PARTIAL` and the cooldown is not guessed |
| Activation | The SDK parameter struct of `UHTAbilitySystemComponent::HTTryActivateAbilityByClass` is `0x10`, and at runtime `UFunction::ParmsSize=0x09`; the class sits at `0x00` and the bool return value at `0x08` | The only mutation entry point in the first release; the return value tells whether the game accepted the request |
| Parameterized activation | `HTServerTryActivateAbilityWithData` takes `0x80` bytes of parameters, including target, section, rotation, direction, position and trigger time | Deferred to a later version; that native struct must not become the ABI directly |

`GetAllAbilities(TArray<...>&)` is not the enumeration entry point in the first release. An output `TArray` produced by ProcessEvent involves UE allocator ownership and destruction, and the host currently has no reusable, stable array ownership bridge. The first release reads the validated, read-only and bounded `ActivatableAbilities.Items` directly, avoiding a cross-allocator free.

## ABI draft

The final declarations go into `include/anomaly/sdk/services/nte.h`. The following describes field and function shapes only; the field order must still be frozen with a Windows x64 ABI snapshot before implementation.

### `anomaly.nte.combat` v1

Suggested capability: `nte-combat-read`.

```c
typedef struct AnomalyNteCombatantSnapshotV1 {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t sequence;
    AnomalyGenerationHandleV1 world;
    AnomalyGenerationHandleV1 character;
    AnomalyGenerationHandleV1 target;
    double hp;
    double max_hp;
    double shield;
} AnomalyNteCombatantSnapshotV1;

typedef struct AnomalyNteDamageEventV1 {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t sequence;
    uint64_t tick_sequence;
    AnomalyGenerationHandleV1 world;
    AnomalyGenerationHandleV1 attacker;
    AnomalyGenerationHandleV1 victim;
    uint64_t source_id;
    int64_t display_damage;
    int64_t basic_damage;
    int64_t final_damage;
    double hit_location[3];
    uint32_t damage_type;
    uint32_t display_type;
    uint32_t reaction_type;
    uint32_t reaction_display_type;
} AnomalyNteDamageEventV1;

typedef struct AnomalyNteCombatStatisticsRequestV1 {
    uint32_t struct_size;
    uint32_t flags;
    AnomalyGenerationHandleV1 world;
    AnomalyGenerationHandleV1 character;
    uint64_t source_id;
    uint32_t direction;
    uint32_t reserved;
} AnomalyNteCombatStatisticsRequestV1;

typedef struct AnomalyNteCombatStatisticsV1 {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t through_sequence;
    uint64_t hit_count;
    uint64_t critical_count;
    uint64_t head_hit_count;
    int64_t display_damage_total;
    int64_t basic_damage_total;
    int64_t final_damage_total;
} AnomalyNteCombatStatisticsV1;

typedef struct AnomalyNteCombatServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *current_combatant)(
        void*, AnomalyNteCombatantSnapshotV1*);
    uint64_t (ANOMALY_CALL *latest_damage_sequence)(void*);
    AnomalyStatusV1 (ANOMALY_CALL *next_damage_event)(
        void*, uint64_t after_sequence, AnomalyNteDamageEventV1*);
    AnomalyStatusV1 (ANOMALY_CALL *statistics)(
        void*, const AnomalyNteCombatStatisticsRequestV1*,
        AnomalyNteCombatStatisticsV1*);
    AnomalyStatusV1 (ANOMALY_CALL *source_name_utf8)(
        void*, uint64_t source_id, char*, size_t*);
    AnomalyStatusV1 (ANOMALY_CALL *participant_path_utf8)(
        void*, AnomalyGenerationHandleV1 participant, char*, size_t*);
} AnomalyNteCombatServiceV1;
```

Conventions:

- Combatant flags include at least `VALID`, `PARTIAL` and `DEAD`; damage flags include `CHARACTER_EVENT` and reserve `CLIENT_PRESENTED`, `CRITICAL`, `HEAD_HIT` and `WEAK_UNBALANCE` for a standalone display event. The two enum sets are defined independently and never reuse bits with unrelated meanings.
- `direction` supports `ANY`, `AS_ATTACKER` and `AS_VICTIM`. A zero character means no participant filter, and a zero source id means no source filter.
- The damage sequence increases monotonically within one host lifecycle; a World change clears the aggregate and leaves a discontinuity in the sequence.
- The ring queue uses a fixed cap. A stale non-zero cursor and the absence of any update both return `NOT_FOUND`, matching the existing session event contract.
- Aggregation uses 64-bit counters and signed 64-bit damage totals, sets `PARTIAL` on overflow and never wraps.
- Statistics cover only normalized events that the ring/aggregate accepted successfully. Parse failures, queue truncation and drop counts go to metrics and are not disguised as complete statistics.
- Damage comes only from the exact native broadcast of `AHTAbilityCharacter::CharacterOnDamaged`; `Damage`, attacker, victim and `DamageGEDef` are read in the same callback and are never correlated with floating text or an RPC afterwards.
- Participant paths are cached while the game thread handles the damage; ABI queries read the cache only and never read a UObject from a plugin UI/Render thread.

### `anomaly.nte.skills` v1

Suggested capability: `nte-skills-read`. The paging shape reuses the generation-pinned iteration semantics of the entities service, with a suggested page cap of 128.

```c
typedef struct AnomalyNteSkillFrameV1 {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t generation;
    uint64_t sequence;
    AnomalyGenerationHandleV1 character;
    uint32_t skill_count;
    uint32_t reserved;
} AnomalyNteSkillFrameV1;

typedef struct AnomalyNteSkillSnapshotV1 {
    uint32_t struct_size;
    uint32_t flags;
    AnomalyGenerationHandleV1 handle;
    AnomalyGenerationHandleV1 character;
    AnomalyGenerationHandleV1 ability_class;
    uint64_t sequence;
    int32_t level;
    int32_t input_id;
    float cooldown_remaining_seconds;
    float cooldown_duration_seconds;
} AnomalyNteSkillSnapshotV1;
```

Service functions:

- `frame`: returns the current immutable skill frame.
- `snapshot_at` / `page`: touch the host cache only and never walk a live UE array on a plugin thread.
- `ability_path_utf8`: resolves the full path of an ability class handle using the buffer sizing contract.
- `snapshot_by_handle`: validates the frame generation of a skill handle and the matching spec identity.

A skill handle is not a direct passthrough of the UE `FGameplayAbilitySpecHandle`. The host should allocate the `id` and keep `{spec handle, ability CDO object handle, class object handle}` in the cache. A refresh extends the identity only while all three still match; removing a skill, re-granting it, switching character or changing World all produce a new generation.

First-release skill flags: `VALID`, `PARTIAL`, `ACTIVE`, `INPUT_PRESSED`, `PENDING_REMOVE`, `REMOVE_AFTER_ACTIVATION`, `COOLDOWN_VALID`. Until there is a purely read-only, validated and generic preflight, no ambiguous `READY` flag is published; the actual acceptance result is returned by the invocation service.

### `anomaly.nte.skill-invocation` v1

Suggested capability: `nte-skill-invocation`. This service is separate from the read-only skill service and continues the mutation isolation pattern of `player-teleport`.

```c
typedef struct AnomalyNteSkillInvocationRequestV1 {
    uint32_t struct_size;
    uint32_t flags;
    AnomalyGenerationHandleV1 world;
    AnomalyGenerationHandleV1 character;
    AnomalyGenerationHandleV1 skill;
} AnomalyNteSkillInvocationRequestV1;

typedef struct AnomalyNteSkillInvocationResultV1 {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t tick_sequence;
    uint32_t accepted;
    uint32_t reserved;
} AnomalyNteSkillInvocationResultV1;
```

Invocation order:

1. Requires the active Game callback domain, otherwise returns `CONFLICT`.
2. Validates the world, character and skill generations; any stale one returns `NOT_FOUND`.
3. Re-reads the current spec and confirms the spec handle, ability CDO and class match the cache.
4. Confirms the class derives from `UHTGameplayAbility` and the spec still belongs to the ASC of the current character.
5. Invokes through the validated `HTTryActivateAbilityByClass` UFunction and the ProcessEvent invoker.
6. Returns `OK` when ProcessEvent executes normally and writes the game bool return value into `accepted`; a transport/reflection invocation failure returns `FAILED`.

Never retry automatically. `accepted == 0` means the game rejected the request and does not indicate a failed bridge call. In the first release `flags` must be 0, and no target, position, direction or remote activation switch is provided.

## Profile and validator design

Suggested new Features:

| Feature | Dependency | validator |
| --- | --- | --- |
| `nte.combat` | `nte.player`, `ue5.names`, `ue5.objects`, `ue5.process-event` and the exact native damage symbol | `nte-combat-reflection-v1` |
| `nte.skills` | `nte.player`, `ue5.names`, `ue5.objects`, `ue5.process-event` | `nte-skills-layout-v1` |
| `nte.skill-invocation` | `nte.skills`, `ue5.process-event` | `nte-skill-invocation-v1` |

The Profile layout must express at least:

- The generic `TArray` data/num/max layout and a reasonable cap.
- `FGameplayAbilitySpecContainer.Items`, the spec stride and the fields read in the first release.
- The `FHTDamageEvent` size, and the offsets of `Damage`, `DamageGEDef` and the weak object index/serial fields.
- When the existing FProperty reflection cannot validate the array inner, add the `FArrayProperty::Inner` layout and validate that generic engine layout first.

The validator must verify:

- The UFunction outer/class, parameter count, `parmsSize`, return offset and the kind/size/offset of every parameter.
- The offset and type of the inherited `Damage` field and the body `DamageGEDef` field of `HTDamageEvent`.
- The reflection shape of the ASC, the spec container, the Items TArray, the spec stride and the fields being read.
- The parameter shapes of `K2_GetAbilitySystemComponent` and `HTTryActivateAbilityByClass`.
- Object pointers must map back to the current GObjects slot with a matching serial; the class/outer inheritance chain has a depth cap.
- The TArray satisfies `0 <= num <= max <= feature limit`, the multiplication and address addition do not overflow, and the whole read range is readable.

Never publish a service just because a Dumper offset matches. The corresponding Feature must stay unavailable when the Profile is missing a field, a reflected object is not loaded, the shape changed, the ProcessEvent ABI did not validate, the Game tick is not established or the character object is stale.

## Runtime ownership and threading

- `NteProfileRuntime` installs exactly one native hook only after the Profile parses and validates `BroadcastCharacterOnDamaged`; it does not go through the global `ProcessEvent`, an Actor vtable or a floating-text entry.
- The native damage callback only reads the validated fields of `FHTDamageEvent`, normalizes participants and source, and writes to a fixed ring; it calls no plugin and performs no file/network I/O.
- The combat aggregate, skill cache and service table belong to the same NTE Profile generation; Stop closes admission first, then withdraws the services and drains the hook callback.
- The current character combat snapshot and skill frames refresh on demand on the Game tick. Plugin queries read the host cache only and never access live UE objects across threads.
- A consumer advances the damage cursor incrementally in the Game-domain `on_update` and resolves names and builds immutable display rows for new events only; the Render-domain `on_draw` copies the local snapshot and calls the UI.
- Skill invocation runs synchronously in the Game callback domain only; it never jumps execution on behalf of the caller from a Render, Lifecycle or Worker thread.
- The service table uses the existing `SemanticServiceEndpoint` call gate. After Stop an old generation table only returns `UNAVAILABLE` and cannot be rebound to the next Start.

## Implementation phases

### Phase 0: offline contract and fixtures

- Turn the Dumper evidence above into a minimal synthetic reflection/object-memory fixture.
- Write separate validator fixtures for `FHTDamageEvent`, weak objects, the ASC/spec container and the two skill UFunctions.
- Verify that the native broadcast template has a single target and pin the argument order of attacker, victim and damage causer.

Exit criteria: a wrong field offset, stride, parameter size, array count, object serial or outer chain must all make the validator fixture fail.

### Phase 1: read-only combat

- Implement the current character combatant cache.
- Implement damage event normalization, the fixed ring, the source name cache and the World-scoped aggregate.
- Publish `anomaly.nte.combat` and add the capability policy, API documentation and metrics.

Exit criteria: World switches, character switches, ring wrap, stale cursors, missing attacker/victim, Miss/immune, negative values and aggregation overflow all have independent validation.

### Phase 2: read-only skills

- Implement the demand-driven skill frame, paging, the opaque handle and the full class path.
- Publish level/input/active state first; publish the cooldown and set `COOLDOWN_VALID` only when the separate sub-gate passes.
- Verify that re-granting the same class after removal does not reuse a stale skill handle.

Exit criteria: character/World/generation switches, spec reordering, duplicate classes, empty abilities, abnormal TArrays and partial cooldown capability all have validation.

### Phase 3: skill invocation

- Publish the separate mutation service and capability.
- Accept only handles returned by the skills service for the current frame.
- Validate argument bytes, the bool return, thread rejection and stale identity through a mock ProcessEvent.

Exit criteria: `accepted=1`, `accepted=0`, invocation failure, a non-Game thread, an old world, an old character, an old skill and a replaced class/spec all have independent results.

### Phase 4: consumers and documentation

- `nte_combat_demo` keeps HP, aggregation, skill paging and opt-in skill invocation while using the correct character damage event path only.
- All service queries, incremental drains, statistics and name resolution live in the Game domain; the Render domain only draws the local snapshot and queues paging or skill invocation intents.
- Update the API reference, the manifest capability table and the developer guide.

## Expected change surface

| Area | Expected content |
| --- | --- |
| `include/anomaly/sdk/services/nte.h` | The three v1 services and the stable enums/structs |
| `apps/abi_snapshot/main.cpp`, `abi/anomaly-sdk-v1-windows-x64.json` | Freeze sizes, alignment, offsets, constants and the service table |
| `src/game/ue5/` | Generic reflection shape checks, object handle conversion and bounded array read primitives |
| `src/game/nte/` | NTE UFunction bindings, the combat ring/aggregate, the skill cache, the invocation gate and lifecycle |
| `profiles/nte/*.json`, `schemas/build-profile.schema.json` | Features, dependencies, validators and the required layout fields; touch the schema only when its shape changes |
| `src/plugin/plugin_capability_policy.cpp` | The three service -> capability mappings and the known capabilities |
| `docs/api-reference/` | The authoritative contract, status codes, threading, cursor, stale and character damage semantics |
| `examples/` | The full combat consumer; character damage capture keeps a single native path |

## Validation matrix

Minimum validation after implementation:

1. Build `anomaly_adapter_services`, `anomaly_nte_profile_runtime`, `anomaly_abi_snapshot`, `anomaly_test_host` and the new example target.
2. Run the validator/object-memory synthetic fixtures, covering correct and incorrect layouts.
3. Run the service contract fixtures, covering paging, cursor, aggregate, generation, call gate and Stop drain.
4. Run `anomaly-abi-snapshot --check abi/anomaly-sdk-v1-windows-x64.json`.
5. Compile the three service tables from separate pure C and C++ external consumers.
6. Run targeted fixtures and degrade-path validation for the native damage hook, nested array parsing, the ring and the Stop/rollback paths.
7. Finally run `git diff --check` and check the API documentation links and capability table consistency.

A live process smoke test does not replace the fixtures above. Verify only when the active Profile already contains the new Feature and a diagnostic session is explicitly scheduled: damage events match what the UI shows, the skill catalog matches the current character, and the `accepted` returned by one invocation matches what the game actually does; after the diagnostic session, leave no background tracker running and no unreviewed log containing object paths.

## Key decisions

- Three services instead of one all-in-one combat table: read-only capabilities and the mutation capability can be authorized, published and degraded independently.
- The first damage release uses the `CharacterOnDamaged` native broadcast: value, participants and `DamageGEDef` come from the same event; display values, criticals and hit locations stay unpublished and are not filled in from floating text.
- Skill enumeration reads the validated internal container instead of calling a reflection function that returns a UE `TArray`, avoiding allocator ownership problems.
- Skill invocation accepts only a host-issued skill handle, never an arbitrary class/path/function, so the invoked object still belongs to the current character.
- Parameterized casting, buffs/GameplayEffects, the full attribute table and skill lifecycle events are extended only after independent evidence exists and are not frozen into v1 early.
