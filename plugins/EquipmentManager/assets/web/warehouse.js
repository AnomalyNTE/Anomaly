import { propertyLabel, qualityLabel, suitLabel } from "./equipment_display.js";
const defaults = { qualities: [], shape: "", suit: "", main: "", sub: [], excluded_sub: [],
  sub_match: "all", min_level: "", max_level: "", min_area: "", max_area: "", sort: "name", open: false };
const mainStats = (item) => item.kind === "core" ? (item.main_stat ? [item.main_stat] : []) : item.main_stats || [];
const qualityOrder = ["Red", "Gold", "Purple", "Blue", "Green", "White"];
const qualityRank = (quality) => { const rank = qualityOrder.indexOf(quality); return rank < 0 ? qualityOrder.length : rank; };
const compareQualities = (left, right) => qualityRank(left) - qualityRank(right) || left.localeCompare(right);

export function createWarehouseFilters({ mount, t, node, inventory, readPreference, savePreference, changed }) {
  let preferences = structuredClone(defaults), loaded = false, signature = "";
  const fields = {};
  const details = node("details", "advanced-filters");
  details.inert = true;
  const summary = node("summary"); summary.dataset.i18n = "web.advanced_filters"; summary.textContent = t("web.advanced_filters");
  const grid = node("div", "advanced-filter-grid");
  const definition = [
    ["qualities", "web.filter_quality", "select", true], ["shape", "web.filter_shape", "select"],
    ["suit", "web.filter_suit", "select"], ["main", "web.filter_main", "select"],
    ["sub", "web.filter_sub", "select", true], ["excluded_sub", "web.filter_excluded_sub", "select", true],
    ["sub_match", "web.filter_sub_match", "select"], ["sort", "web.filter_sort", "select"],
    ["min_level", "web.filter_min_level", "number"], ["max_level", "web.filter_max_level", "number"],
    ["min_area", "web.filter_min_area", "number"], ["max_area", "web.filter_max_area", "number"],
  ];
  for (const [key, caption, type, multiple] of definition) {
    const label = node("label"), text = node("span", "", t(caption)); text.dataset.i18n = caption;
    const control = node(type === "select" ? "select" : "input");
    if (multiple) { control.multiple = true; control.size = 3; }
    if (type === "number") { control.type = "number"; control.min = "0"; control.step = "1"; }
    control.onchange = () => {
      if (type === "number" && !control.checkValidity()) return;
      preferences[key] = multiple ? [...control.selectedOptions].map((option) => option.value) : control.value;
      persist();
    };
    fields[key] = control; label.append(text, control); grid.append(label);
  }
  details.append(summary, grid); mount.append(details);
  details.ontoggle = () => { if (loaded && preferences.open !== details.open) { preferences.open = details.open; persist(); } };

  function persist() { savePreference(JSON.stringify(preferences)); changed(); }
  function options(key, values, selected, multiple = false) {
    fields[key].replaceChildren(...values.map(([value, caption]) => {
      const option = node("option", "", caption); option.value = value;
      option.selected = multiple ? selected.includes(value) : value === selected;
      return option;
    }));
  }
  function sync() {
    if (!loaded) {
      const saved = readPreference();
      if (saved) {
        try {
          const candidate = { ...preferences, ...JSON.parse(saved) };
          if (!["qualities", "sub", "excluded_sub"].every((key) => Array.isArray(candidate[key]) && candidate[key].every((id) => typeof id === "string"))) throw new Error();
          preferences = candidate;
        } catch { /* Invalid saved filters remain untouched until the user edits them. */ }
      }
      for (const key of ["min_level", "max_level", "min_area", "max_area"]) fields[key].value = preferences[key];
      details.open = preferences.open === true;
      loaded = true;
      details.inert = false;
    }
    const items = inventory()?.items || [], qualities = new Set(preferences.qualities), shapes = new Set(), suits = new Map(), main = new Map(), sub = new Map();
    if (preferences.shape) shapes.add(preferences.shape);
    if (preferences.suit) suits.set(preferences.suit, t("web.unknown_suit"));
    if (preferences.main) main.set(preferences.main, propertyLabel(t, preferences.main));
    for (const id of [...preferences.sub, ...preferences.excluded_sub]) sub.set(id, propertyLabel(t, id));
    for (const item of items) {
      if (item.quality) qualities.add(item.quality);
      if (item.kind === "module" && item.shape_id) shapes.add(item.shape_id);
      if (item.suit_id) suits.set(item.suit_id, suitLabel(t, item));
      for (const stat of mainStats(item)) main.set(stat.id, propertyLabel(t, stat));
      for (const stat of item.sub_stats || (item.attributes || []).filter((value) => value.source !== "main")) sub.set(stat.id, propertyLabel(t, stat));
    }
    const next = JSON.stringify([[...qualities].sort(), [...shapes].sort(), [...suits].sort(), [...main].sort(), [...sub].sort()]);
    if (next === signature) return;
    signature = next;
    const any = [["", t("web.filter_any")]];
    options("qualities", [...qualities].sort(compareQualities).map((id) => [id, qualityLabel(t, id)]), preferences.qualities, true);
    options("shape", [...any, ...[...shapes].sort().map((id) => [id, id])], preferences.shape);
    options("suit", [...any, ...suits], preferences.suit);
    options("main", [...any, ...main], preferences.main);
    options("sub", [...sub], preferences.sub, true); options("excluded_sub", [...sub], preferences.excluded_sub, true);
    options("sub_match", [["all", t("web.filter_all_stats")], ["any", t("web.filter_any_stats")]], preferences.sub_match);
    options("sort", [["name", t("web.sort_quality_name")], ["level_desc", t("web.sort_level_desc")], ["level_asc", t("web.sort_level_asc")]], preferences.sort);
  }

  function matches(item) {
    if (!loaded) return true;
    const p = preferences;
    if (p.qualities.length && !p.qualities.includes(item.quality)) return false;
    if (p.shape && (item.kind !== "module" || item.shape_id !== p.shape)) return false;
    if (p.suit && item.suit_id !== p.suit) return false;
    const mains = mainStats(item);
    if (p.main && !mains.some((stat) => stat.id === p.main)) return false;
    const ids = new Set((item.sub_stats || (item.attributes || []).filter((stat) => stat.source !== "main")).map((stat) => stat.id));
    if (p.sub.length && !(p.sub_match === "any" ? p.sub.some((id) => ids.has(id)) : p.sub.every((id) => ids.has(id)))) return false;
    if (p.excluded_sub.some((id) => ids.has(id))) return false;
    for (const [lower, upper, value] of [[p.min_level, p.max_level, item.level], [p.min_area, p.max_area, item.kind === "module" && item.cells?.length ? item.cells.length : null]]) {
      if ((lower !== "" || upper !== "") && typeof value !== "number") return false;
      if (lower !== "" && value < Number(lower)) return false;
      if (upper !== "" && value > Number(upper)) return false;
    }
    return true;
  }
  return {
    sync, matches,
    compare(left, right) {
      if (["level_desc", "level_asc"].includes(preferences.sort)) {
        const leftKnown = typeof left.level === "number", rightKnown = typeof right.level === "number";
        if (leftKnown !== rightKnown) return leftKnown ? -1 : 1;
        if (leftKnown && left.level !== right.level) return (left.level - right.level) * (preferences.sort === "level_desc" ? -1 : 1);
      }
      return qualityRank(left.quality) - qualityRank(right.quality) ||
        String(left.name || left.item_id || "").localeCompare(String(right.name || right.item_id || "")) ||
        left.uid.slot - right.uid.slot || left.uid.serial - right.uid.serial;
    },
    reset() { preferences = structuredClone(defaults); signature = ""; for (const key of ["min_level", "max_level", "min_area", "max_area"]) fields[key].value = ""; details.open = false; sync(); persist(); },
  };
}
