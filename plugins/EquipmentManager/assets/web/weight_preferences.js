// Scoring weight presets and remote/local recommended weight catalog.
// Recommended character weights originate from Yihuan Workshop (异环工坊):
// fetched from the live API on plugin startup, with GitHub rawcontent and the
// bundled local static file as fallbacks.
export const generalOutputWeights = Object.freeze({ DamageUpGeneralBase: 0.75, CritBase: 1, CritDamageBase: 1, AtkUp: 0.7 });
const optionalFields = ["required_shapes", "crit_minimum", "crit_maximum", "crit_minimum_base", "crit_maximum_base", "extra_shape"];

const defaultGeneral = Object.freeze({
  weights: generalOutputWeights,
  main_weights: generalOutputWeights,
});

let recommendedCatalog = {
  schema_version: 1,
  source: "builtin",
  attribution: "",
  generated_at: "",
  origin: "builtin",
  status: "idle",
  revision: 0,
  remote_url: "",
  remote_error: "",
  general: defaultGeneral,
  characters: {},
};

function validWeightMap(value, requirePositive = false) {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const entries = Object.entries(value);
  if (entries.length > 128) return null;
  const normalized = {};
  let hasPositive = false;
  for (const [id, weight] of entries) {
    if (typeof id !== "string" || !id || id.length > 128) return null;
    if (typeof weight !== "number" || !Number.isFinite(weight) || weight < 0 || weight > 1) return null;
    if (weight > 0) {
      normalized[id] = weight;
      hasPositive = true;
    }
  }
  if (requirePositive && !hasPositive) return null;
  return normalized;
}

export function normalizeRecommendedCatalog(raw) {
  if (!raw || typeof raw !== "object" || Array.isArray(raw) || raw.schema_version !== 1) return null;
  const generalWeightsMap = validWeightMap(raw.general?.weights, true) || { ...generalOutputWeights };
  const generalMainMap = validWeightMap(raw.general?.main_weights, false);
  const characters = {};
  if (raw.characters && typeof raw.characters === "object" && !Array.isArray(raw.characters)) {
    for (const [id, entry] of Object.entries(raw.characters)) {
      if (typeof id !== "string" || !id || !entry || typeof entry !== "object" || Array.isArray(entry)) continue;
      const weights = validWeightMap(entry.weights, true);
      if (!weights) continue;
      const mainWeights = validWeightMap(entry.main_weights, false);
      characters[id] = {
        ...(typeof entry.name === "string" && entry.name ? { name: entry.name } : {}),
        weights,
        main_weights: mainWeights && Object.keys(mainWeights).length ? mainWeights : { ...weights },
      };
    }
  }
  return {
    schema_version: 1,
    source: typeof raw.source === "string" ? raw.source : "builtin",
    attribution: typeof raw.attribution === "string" ? raw.attribution : "",
    generated_at: typeof raw.generated_at === "string" ? raw.generated_at : "",
    origin: ["api", "github", "local", "builtin"].includes(raw.origin) ? raw.origin : (Object.keys(characters).length ? "local" : "builtin"),
    status: ["idle", "fetching", "ready", "fallback"].includes(raw.status) ? raw.status : "idle",
    revision: Number.isFinite(raw.revision) ? raw.revision : 0,
    api_url: typeof raw.api_url === "string" ? raw.api_url : "",
    rawcontent_url: typeof raw.rawcontent_url === "string" ? raw.rawcontent_url : "",
    remote_error: typeof raw.remote_error === "string" ? raw.remote_error : "",
    general: {
      weights: generalWeightsMap,
      main_weights: generalMainMap && Object.keys(generalMainMap).length ? generalMainMap : { ...generalWeightsMap },
    },
    characters,
  };
}

export function setRecommendedWeightsCatalog(raw) {
  const normalized = normalizeRecommendedCatalog(raw);
  if (!normalized) return false;
  const changed = JSON.stringify(recommendedCatalog) !== JSON.stringify(normalized);
  recommendedCatalog = normalized;
  return changed;
}

export function getRecommendedWeightsCatalog() {
  return recommendedCatalog;
}

export function generalWeights() {
  return {
    weights: { ...(recommendedCatalog.general?.weights || generalOutputWeights) },
    main_weights: { ...(recommendedCatalog.general?.main_weights || recommendedCatalog.general?.weights || generalOutputWeights) },
  };
}

export function recommendedWeightsForRole(role) {
  if (!role || role.official === false) return null;
  const candidates = [role.id, role.observed?.item_id].filter((id) => typeof id === "string" && id.length > 0);
  for (const id of candidates) {
    const entry = recommendedCatalog.characters?.[id];
    if (entry && Object.values(entry.weights || {}).some((weight) => weight > 0)) {
      return {
        ...(entry.name ? { name: entry.name } : {}),
        weights: { ...entry.weights },
        main_weights: { ...(entry.main_weights || entry.weights) },
      };
    }
  }
  return null;
}

export function recommendedCoreForRole(role) {
  if (!role || role.official === false) return null;
  const core = role.observed?.recommended_core || role.recommended_core;
  return core && typeof core.suit_id === "string" && core.suit_id ? core : null;
}

export function resolveCorePreference(role, preference) {
  const manual = role?.official === false || preference.core_suit_source === "manual" ||
    (preference.core_suit_source == null && Boolean(preference.core_suit_id));
  return { ...preference, core_suit_source: manual ? "manual" : "recommended",
    core_suit_id: manual ? preference.core_suit_id || "" : recommendedCoreForRole(role)?.suit_id || "" };
}

export function rolePreference(role = {}) {
  const preference = { weights: role.weights || {}, main_weights: role.main_weights || role.weights || {}, shared_main_weights: false,
    require_core: role.require_core !== false, set_effect_mode: role.set_effect_mode || "four_piece", core_suit_id: role.core_suit_id || "", core_main_stats: role.core_main_stats || [],
    core_suit_source: role.core_suit_source || (role.core_suit_id || role.official === false ? "manual" : "recommended"),
    priority_stats: role.priority_stats || [], priority_equal: role.priority_equal === true, priority_min_grade: role.priority_min_grade || "A",
    filters: role.filters || {}, property_limits: role.property_limits || [], allow_missing_core: role.allow_missing_core === true,
    include_discarded: role.include_discarded === true, allow_borrow: role.allow_borrow !== false };
  for (const field of optionalFields) if (role[field] != null) preference[field] = role[field];
  return structuredClone(preference);
}

function generalPreference(preference) {
  const general = generalWeights();
  return { ...preference, weights: general.weights, main_weights: general.main_weights,
    shared_main_weights: JSON.stringify(general.weights) === JSON.stringify(general.main_weights), preset: "general" };
}

function recommendedPreference(preference, recommended) {
  return { ...preference, weights: { ...recommended.weights }, main_weights: { ...recommended.main_weights },
    shared_main_weights: false, preset: "recommended" };
}

function scoringPreference(role, stored, initial) {
  const recommended = recommendedWeightsForRole(role);
  if (stored) {
    const resolved = structuredClone(stored);
    const preset = stored.preset ?? (Object.hasOwn(stored, "weights") || Object.hasOwn(stored, "main_weights") ? "custom" : "general");
    if (preset === "recommended") {
      if (recommended) return recommendedPreference(resolved, recommended);
      if (Object.values(resolved.weights || {}).some((weight) => weight > 0)) return { ...resolved, preset: "custom" };
      return generalPreference(resolved);
    }
    if (preset === "general") return generalPreference(resolved);
    if (preset !== "role") return { ...resolved, preset };
    for (const field of optionalFields) delete resolved[field];
    return { ...resolved, ...rolePreference(role), preset: "role" };
  }
  if (Object.values(role.weights || {}).some((weight) => weight > 0)) return { ...rolePreference(role), preset: "role" };
  if (recommended) return recommendedPreference(rolePreference(role), recommended);
  if (initial) return { ...structuredClone(initial), preset: initial.preset ?? "custom" };
  return generalPreference(rolePreference(role));
}

export function resolveOptimizationPreference(role = {}, stored, initial = null) {
  return resolveCorePreference(role, scoringPreference(role, stored, initial));
}

// Single and team requests use the same scoring preferences and raw units.
// An empty suit means "any suit", so it must not enter the nonempty-ID field.
export function optimizationPreferenceFields(preference, items = [], character = null) {
  const result = {
    weights: structuredClone(preference.weights || {}), require_core: preference.require_core !== false,
    set_effect_mode: preference.require_core === false ? "none" : preference.set_effect_mode || "four_piece",
    include_discarded: preference.include_discarded === true, allow_borrow: preference.allow_borrow !== false,
    priority_stats: structuredClone(preference.priority_stats || []), priority_equal: preference.priority_equal === true,
    priority_min_grade: preference.priority_min_grade || "A",
  };
  if (preference.shared_main_weights !== true) result.main_weights = structuredClone(preference.main_weights || preference.weights || {});
  if (result.require_core && preference.core_suit_id) result.core_suit_id = preference.core_suit_id;
  if (result.require_core && preference.core_main_stats?.length) result.core_main_stats = structuredClone(preference.core_main_stats);
  if (preference.filters) result.filters = structuredClone(preference.filters);
  if (preference.property_limits?.length) result.property_limits = structuredClone(preference.property_limits);
  for (const field of optionalFields) if (preference[field] != null) result[field] = structuredClone(preference[field]);
  if (preference.allow_missing_core === true) {
    result.allow_missing_core = true;
    if (result.require_core && result.set_effect_mode !== "none") {
      const definition = items.find((item) => item.kind === "core" && item.suit_id === preference.core_suit_id && item.required_shapes_complete === true);
      if (definition) result.required_shapes = structuredClone(definition.required_shapes);
    }
  }
  if (preference.keep_current && character) {
    const equipped = items.filter((item) => item.character?.slot === character.slot && item.character?.serial === character.serial);
    result.keep = { placements: equipped.filter((item) => item.kind === "module")
      .map((item) => ({ equipment: item.uid, row: item.row, column: item.column })) };
    const core = result.require_core && equipped.find((item) => item.kind === "core");
    if (core) result.keep.core = core.uid;
  }
  return result;
}
