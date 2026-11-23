# Character and cultivation mathematics

`character_math.hpp` contains the pure character and cultivation functions. They consume caller-owned,
frozen JSON and never query a catalog, read a game object, write state, or issue
RPC. All percentages are native fractions. A complete source assertion belongs
to the producer that joined the profile to verified catalog rows. Missing
profile values, conditional modifiers, unrecognized operations, and missing
curves cannot be hidden in an empty complete source.

The persistent `role.profile` remains an input to the producer, not a math
schema: `character_level`, `character_breakthrough_stage`, `fork_id`,
`fork_level`, `fork_breakthrough_stage`, `fork_refinement`, `awakening_level`,
`enabled_awakening_ids`, `skill_levels`, `likeability_10`, and
`furniture:{AtkAdd,CritDamageBase}`. Null means unset. The producer resolves the
selected level and stage exactly and includes only verified unconditional
contributions in sources. An observed current combat panel, which already
contains equipment/buffs, is not a permanent base source.

## CalculateCharacter

```json
{
  "character_level": 80,
  "required_sources": ["character", "fork", "furniture"],
  "sources": [
    {"id":"character","complete":true,"attributes":{"AtkBase":100,"HPMaxBase":1000,"DefBase":100,"CritBase":0.05,"CritDamageBase":0.5}},
    {"id":"fork","complete":true,"attributes":{}},
    {"id":"furniture","complete":true,"attributes":{"AtkAdd":20,"CritDamageBase":0.04}}
  ],
  "equipment": [],
  "damage": {
    "property":"DamageUpChaosBase",
    "enemy":{"level":80,"scene":"outer_realm","resistance":0.2}
  }
}
```

The numbers above illustrate the formula contract; they are not a fallback for
an unknown live character. All sources require unique `id`, `complete:true`,
and an `attributes` object of finite native values. `required_sources` checks
the producer's declared provenance. `AtkBase`, `HPMaxBase`, `DefBase`,
`CritBase`, and `CritDamageBase` must be present in their sum. Intrinsic crit is
included by the producer exactly once; the calculator does not add another 5%
or 50%. Known no-fork/no-furniture states use explicit complete empty sources.

Equipment has `kind`, `main_stats_complete:true`, and
`sub_stats_values_complete:true`. Attribute rows use `id` and raw `value`;
verified `max_value` takes precedence for main properties, matching the
reference's maximum-level equipment projection. Cores use `main_stat`; modules
use `main_stats`. Optional `extra_shape:{area,buffs:{property:raw}}` adds buffs
once per matching-area module. Source IDs `equipment` and `extra_shape` cannot
also be supplied in `sources` when their separate input is present.

The result exposes raw `attributes`, `derived` attack/health/defense/crit,
source breakdown, and optional `direct_damage`, `margins`, `dynamic_weights`,
`graduation`, and `search`. Derived attributes follow
`base*(1+increase)+additive`. The 100% ATK representative hit follows the
reference's actual `DamageCalculationService` and official-role services:
general plus selected elemental damage, expected crit, enemy defense,
resistance, vulnerability, and independent factors. It is not the legacy coarse
`damage_model.py` shortcut, and does not floor a role-panel expectation.

Each source breakdown preserves the producer's metadata, including complete,
unconditional, operation assertions, and dataset/row provenance. Its
`attributes` is the calculated contribution to the final panel; with an
override modifier this is the actual delta, not an additional copy of the
overridden value. Modifiers are returned in their stable application order.
The producer's `attributes_metadata:[{id,name,percent}]`,
`attributes_metadata_source`, and `attributes_metadata_scope` are also
preserved by the character, contribution, and replacement functions. These
describe the verified subset of properties; an absent row is unknown, not a
reason to infer a property's display unit from its identifier.

Enemy `scene` is `outer_realm` or `open_world`, with the reference's defense
offsets 90/100. Alternatively provide `defense_base`, optional `defense_up` and
`defense_add`; the pack-derived result is divided by six. Optional fields are
`defense_reduction`, `resistance_reductions`, `resistance_penetrations`, and
`vulnerability_increases`. Their missing values mean the declared model has no
such modifier. Enemy `resistance` and either its level/scene or defense pack
are mandatory; an unobserved target is never supplied as an observed one.

`damage.crit_cap` is optional; when present it is a fraction in 0..1. Without
it, the calculation preserves the actual reference service's uncapped input
semantics. `damage.independent_bonuses` is an optional list of unconditional
independent increases. No skill rotation, conditional passive, or battle replay
is modeled here.

The standard marginal units are native `.01 CritBase`, `.02 CritDamageBase`,
`.01 DamageUpGeneralBase`, `.0125 AtkUp`, `8 AtkAdd`, and `.0125` for the chosen
element property. `benefit_units` can override these entries. `base_weights`
and optional `base_main_weights` preserve non-formula property weights
separately in `dynamic_weights` and `dynamic_main_weights` (the main base
defaults to the sub base when omitted); participating properties normalize
their positive gains against the largest gain, retaining true zero gains as
zero. A zero baseline damage makes relative margins unavailable.
Crit and general-damage marginal current values include their native additive
counterparts, matching the inputs used by the damage formula.

### Graduation

`graduation.baseline` is another complete character request frozen at maximum
non-equipment progression, using the actual equipment. Both numerator and
denominator reuse these same sources; current lower progression must not enter
the numerator. A supplied `graduation.benchmark` is an explicit equipment array;
optional `benchmark_extra_shape` replaces its extra-shape input.

Alternatively supply `graduation.catalog`, `weights`, and optional
`main_weights`. The verified catalog has `module_area`, `module_main_stats`,
`gold_sub_values_per_cell`, `core_sub_values`, `core_main_values`, optional
`default_core_main`, and optional `extra_buffs` plus `extra_count`. The template
selects the four highest weighted allowed sub-properties, ties by ID. It
evaluates positively weighted core main choices against the full-progression
damage formula and chooses the best. All template values come from the supplied
catalog. Its aggregate benchmark contains no fabricated UID and is marked
`native_assemblable:false`.

Graduation returns `actual_damage`, `benchmark_damage`, and their raw `rate`
(multiply by 100 only in display). The ratio is not capped at one.

## Equipment contribution and replacement

`CalculateEquipmentContributions(model, cancel_check)` accepts the same frozen
character model, with a complete `equipment` array containing unique real UIDs.
It returns `equipment_gain:{available,damage,baseline_damage,gain_percent}` for
the whole loadout versus no equipment, plus `items` with the same fields and
`equipment` UID for removing each item from that same loadout. Extra-shape
piece counts are recalculated after removal. A zero baseline makes a relative
gain undefined, reported as null with `available:false`. The gain is a display
percentage, so 10 means ten percent. No item or saved plan is modified.

`RankEquipmentReplacements(request, frozen_inventory, cancel_check)` accepts:

```json
{
  "snapshot_id":"generation:revision",
  "equipment":{"slot":1,"serial":1},
  "model":{"sources":[],"equipment":[],"character_level":80,"damage":{}},
  "weights":{},
  "main_weights":{},
  "exclude_uids":[],
  "allow_borrow":true,
  "include_discarded":false,
  "limit":200
}
```

The abbreviated model above must be the complete normalized character model
described earlier. Its target UID must appear in `model.equipment`. Every
selected item's attributes are taken from the same frozen inventory; the
model cannot override them. `weights`/`main_weights` optionally replace the
model's `base_weights`/`base_main_weights`. Graduation is not recalculated.

The current full loadout determines final marginal weights once. These same
weights feed `ScoreEquipment` for both the current item and every candidate.
Candidates must be outside the selected loadout and reservations. Modules must
have the identical formal shape and signed cells, preserving the RPC anchor;
cores must have the same formal suit and required-shape multiset. Discarded
items are excluded by default. With `allow_borrow:false`, equipped items are
excluded unless their owner equals the optional native `character` UID.

The output contains `source_snapshot_id`, `equipment`, `baseline_damage` (the
current full loadout), `without_item_damage`, `current_score`,
`current_direct_damage_score`, the two dynamic weight maps, and `candidates`.
Each candidate has its real `equipment` UID, `score`, `grade`, `damage`,
`gain_percent` versus the current full loadout, and `direct_damage_score` versus
the loadout without the target. Ranking follows the reference: score descending,
then actual damage descending, then stable UID. Direct-damage gain remains a
separate metric; nonpositive candidates are still returned.

`limit` is 1..500 (default 200). `total` counts evaluated valid candidates before
that display limit, and `truncated` identifies its effect. Data errors are local
`errors:[{equipment,error_code}]`; missing values never become zero-score
candidates. `complete:false` identifies unavailable candidate data or an
interrupted bounded scan. `search` describes the one shared budget used for the
baseline, marginal weights, and candidates. Cancellation throws the usual
stable cancellation failure. A budget that expires before a baseline exists
throws budget exhaustion; a later stop retains the explicitly partial ranking.

## CalculateExperienceMaterials

Input is `{required_experience,materials:[{item_id,experience,gold_cost?}]}`.
The integer optimizer minimizes experience overflow first, material count
second, then uses descending denomination/stable ID ties. Optional unknown
`gold_cost` remains null. Output contains material quantities, `overflow`,
`item_count`, `gold_cost`, `known_gold`, `status`, and `search`.
A zero requirement returns a complete empty selection without requiring a
material catalog.

The DP uses a GCD scale and a proven bound smaller than one maximum
denomination above the requirement. It is bounded before allocating memory;
search exhaustion returns an unavailable result with `optimization_limit`, not
a greedy estimate presented as optimal.

## CalculateCultivation

Input plans use exact normalized transitions rather than ambiguous native row
numbers. The data adapter must verify whether a table row describes the
current or destination level before constructing these edges.

```json
{
  "plans":[{
    "id":"role-1",
    "tracks":[{
      "id":"character-level","current":1,"target":3,"already_experience":0,
      "steps":[
        {"from":1,"to":2,"experience":100,"items":{},"gold":5},
        {"from":2,"to":3,"experience":200,"items":{},"gold":5}
      ],
      "experience_materials":[{"item_id":"book","experience":100,"gold_cost":2}]
    }]
  }],
  "owned":{"book":2,"Gold":100},
  "allow_crafting":true,
  "recipes":[]
}
```

Character/fork levels, breakthroughs, skills, awakenings, and refinements are
separate named tracks. Each required transition must exist exactly once and
advance toward the target. Optional `requires:[{track,minimum}]` checks target
prerequisites. `items` are exact material quantities and `gold` is an explicit
nonnegative quantity or null. Unknown Gold is never replaced by zero. Current
experience is deducted once before choosing experience materials.

The producer may identify unknown tracks with `plan.incomplete_tracks:[id]`,
`track.complete:false`, or null/missing current or target values. Missing
required edges, null items/experience, unknown already-earned experience when
an experience cost is required, and missing experience denominations also make
that track unavailable. The function retains other verified tracks instead of
discarding the whole request. A track is accumulated atomically: costs from a
partially resolved track are not represented as its full cost. A known
current-equals-target track requires no edges and has zero cost.

Plans share one owned-stock map in caller order. Each owned material is consumed
once; lower-tier requirements are fulfilled before higher crafting in a plan.
Only the reference's explicitly allowlisted material families can use 3:1
conversion: EquipmentUpMaterial, OrdinaryMonMaterial_01..04,
SkillUpMaterial_01..05, and WeaponBreakMaterial_01..05, tiers `_lv1`..`_lv3`.
Experience denominations are never converted by quality.

Recipes are explicit `{from,to,ratio:3,gold_cost}` rows from verified recipe
data. Each tier step pays its Gold cost, including intermediate crafting. A
missing/unknown recipe is not a free action. Currency is `Gold`; parsing native
raw tokens such as exact `gold` belongs to the adapter. Fons remains a separate
ID and cannot silently become Gold or stamina.

Output has per-plan materials, owned allocations, deficits, crafts, tracks,
nullable total Gold, known Gold, and remaining stock; the aggregate exposes
`base_materials` separately so a farming calculation does not count crafting
Gold twice. Optional `stamina` attaches a farming request to the same total
budget and frozen stock. Unknown costs keep its total stamina null.

Computed plans retain the producer's role/source metadata. Computed tracks
retain current/target, provenance, row/cost semantics, experience scope, and
other metadata, including for unavailable tracks. Raw `steps` and the candidate
experience denomination catalog are not duplicated in results; the selected
experience materials replace that input field when needed. In particular,
`full_level_interval` stays distinct from `observed_remaining_interval`.

An unavailable track has `status:"unavailable"`, null materials/Gold/experience,
and `error_code:"algorithm.incomplete_attributes"`. The overall result is
`partial`, `materials_complete:false`, with null full `materials`, `gold`, and
`remaining_owned`. `known_materials`, `known_gold`, and per-plan
`known_deficits` retain verified subtotals; they are never claimed to cover the
unknown track. Stock allocation continues over known costs in caller order,
with `owned_allocation_complete:false` after any incomplete earlier cost.
`unallocated_owned_after_known_costs` names that qualified stock simulation.
Unknown Gold alone can leave non-Gold material requirements complete while its
Gold entry remains null. Attached farming keeps `total_stamina:null` and reports
`gold_cost_unavailable` and/or `cultivation_cost_unavailable` gaps.

Explicit `owned:null` or `owned_complete:false` means account stock is unknown;
it never becomes an empty stock assertion. Verified upgrade requirements and
their known Gold remain available, but the result is partial. Per-plan owned
allocation, all after-owned deficits, crafting actions/costs, remaining stock,
and its qualified simulation are null. No crafting is inferred without stock;
reported upgrade Gold excludes any as-yet-unplanned crafting. Attached farming
is unavailable with null known/total stamina and gap
`owned_materials_unavailable`. Omitting `owned` retains the original explicit
empty-stock calculation contract.

### Separate and combined stamina

Optional `compare_stamina:true` requires a `stamina` model. The combined solve
runs first; each plan and track then receives a `stamina` result under the same
remaining job budget. A plan uses only its already allocated material credits,
so account stock is not reused by another plan. Tracks consume those credits
in input order. Crafting Gold already spent to create an allocated material is
not credited a second time as available currency.

`separate_stamina_total` sums complete per-plan totals. `saved_stamina` is
`max(0,separate_stamina_total - stamina.total_stamina)` only when both are known.
These are never replaced by the sum of incomplete estimates.
`known_separate_stamina` is the explicit known subtotal, or null when none was
calculable. `comparison_complete` additionally requires complete per-track
results. A late comparison budget stop keeps the combined result and marks the
remaining comparisons unavailable with `optimization_limit`.

`source_ledger` records `plan_id`, `track_id`, `item_id`, required quantity,
allocated owned quantity, and remaining quantity. Unknown allocation fields
are null, with `source_ledger_complete:false`; they are not zero stock
assertions. This ledger describes upgrade requirements; actual crafting
actions and their currency costs remain in each plan's `crafts`.

## CalculateStamina

Input is `{hunter_level,effective_identification_level?,requirements:{id:q},owned?,allow_crafting?,recipes?,stages:[...]}`.
Each stage supplies `stage_id`, `minimum_hunter_level`,
`minimum_identification_level`, positive `stamina_cost`, deterministic integer
`yields:{id:q}`, and a `source`. No expected/random drops are manufactured.

The default `scope:"all_materials"` retains every requested material. Explicit
`scope:"cultivation_material_stages"` applies the reference cultivation
projection: Fons is outside scope; `OrdinaryMonMaterial_` and
`Worldboss_material_` IDs are outside scope only when no supplied stage yields
that exact ID. Gold is always retained, even if its yield is unavailable.
Excluded requirements remain visible in `outside_stamina_materials`, and stay
in cultivation material totals. Accessibility still uses the selected level
gates; the scope check uses all supplied model stages as in the reference.

The reference identification thresholds are 10/20/30/40/45/50/55 over hunter
levels 1..60. Effective identification can remain native or, from native level
three, decrease by one. Only accessible stages enter the optimizer. The bounded
Dijkstra search minimizes stamina, then run count. Its state includes enough
lower-tier material and Gold for verified crafting; owned lower material can
combine with future Gold drops. Intermediate crafts preserve their exact cost.

Output matches the domain distinctions `complete`, `partial`, and `unavailable`:
known stamina, nullable total stamina, material deficits, selected runs,
produced quantities, recipe actions, unresolved IDs, and machine gap codes.
Missing deterministic yields or recipes never produce a fabricated zero total.
Search exhaustion is `optimization_limit`. The caller localizes these codes.

All functions share the same maximum three-second budget and cancellation
callback. Limits belong to the whole multi-plan request, including experience
selection, stock allocation, and farming. A budget exception is the stable
`algorithm.budget_exhausted`; caller cancellation remains `algorithm.cancelled`.
