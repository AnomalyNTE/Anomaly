# Reference stamina model

`ReferenceStaminaModel()` in `reference_stamina_model.hpp` returns a caller-owned
JSON model. It is built from compile-time data in `reference_stamina_model.cpp`;
there is no production SQLite dependency, reference-repository path lookup,
network request, or live game access. Call it on Worker and freeze the returned
`stages`/`scope` with the calculation request. Add only
`reference_stamina_model.cpp` to the plugin target.

The stable model ID is `reference-20260924-v49`, JSON schema version 1. It is an
explicit reference model rather than an observation of the current game:

- Reference repository: `NTE-Drive-Calculator`.
- Source: its tracked `data/role_catalog/game_static.sqlite3`.
- Dataset: `cn_retail_reference_20260924_9030b45b`, importer 49, static schema 38.
- Database SHA-256:
  `500414eadb0eb59710271d5d064616523089eacb588d05bb0d784e570e0dd524`.
- Canonical exported stages SHA-256:
  `d733dc10aeff01083ef4510f560d6ce1fda9cde4d5513c696abb56026a76c434`.

The stage digest covers the exported `stages` array as UTF-8 JSON, recursively
sorted keys, ASCII escapes, and no whitespace between tokens. Array order is
the reference DAO's clone ID then difficulty ordinal order. Source-file hashes
and paths for CloneSystem, ClientDropGroup, and DropSequence are included in
the model. The reference package omits original row payloads; its metadata and
normalized closure remain attributed to that dataset. Original game IDs are
retained as data. The table contains no copied display labels; callers render
model/assumption labels through plugin localization and may display raw IDs.

The extraction follows the real
`StaticGameDataProgressionQueriesMixin.list_progression_farming_stages` query:

1. Join `clone_activity`, `clone_activity_difficulty`,
   `clone_drop_projection`, and `clone_drop_projection_item`.
2. Require positive stamina and a complete/partial projection.
3. Reject any drop with a `clone_drop_projection_gap` other than `name_missing`.
4. Keep only positive integer item quantities and nonempty yield bundles.

This produces 148 stages and 1192 stage-yield entries. Shared drop definitions
can appear under more than one stage. The source's main static database yields
the same normalized stage model. Every stage keeps its formal clone/drop ID,
type, visibility flag, level gates, stamina, whole yield bundle, and closure
status. No random mean, range average, or missing quantity is synthesized.

`policy` preserves the reference's hunter levels 1..60, identification
thresholds `[10,20,30,40,45,50,55]`, and the one-level reduction permitted from
native identification level three. These values match the supported pure
planner policy; they are not read from a live account.

`scope` is `cultivation_material_stages`, as defined in `character_math.md`.
Fons stays outside this scope. Ordinary-monster/world-boss material IDs remain
in cultivation totals but only enter this stamina scope when a supplied stage
yields them explicitly. Gold always remains required. Importer 49 already
uses Gold; no legacy Fons conversion is applied.

The reference query includes 21 weekly stages and three adventure-hidden Trial
stages. Its availability model uses level gates only. Accordingly the exported
assumptions explicitly state that current-game equivalence, weekly reward
limits, and quest unlocks are unverified/unmodeled; adventure visibility is not
used as a filter. A result is an exact calculation within this supplied model,
not a claim that every returned run is currently repeatable or unlocked.

Temporary pure verification executes the actual reference DAO against its
read-only catalog and compares every exported stage, quantity, gate, hash, all
60 hunter-level projections, and a model-to-planner contract. This is a static
model comparison, not an offline test of game behavior.
