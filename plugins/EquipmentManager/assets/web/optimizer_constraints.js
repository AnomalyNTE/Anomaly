import { propertyLabel, qualityLabel } from "./equipment_display.js";
export function createOptimizerConstraints({ mount, t, node, inventory, preferences, changed }) {
  let signature = "";
  const details = node("details", "optimizer-section optimizer-constraints"); details.append(node("summary", "", t("web.constraints_title")));
  const content = node("div", "constraint-content"), filters = node("div", "constraint-filters"), controls = new Map();
  const stats = () => {
    const values = new Map();
    for (const item of inventory()?.items || []) for (const stat of [...(item.sub_stats || []), ...(item.main_stats || []), ...(item.main_stat ? [item.main_stat] : [])]) if (stat.id) values.set(stat.id, { id: stat.id, name: propertyLabel(t, stat), percent: stat.percent === true });
    return values;
  };
  for (const kind of ["module", "core"]) {
    const section = node("section", "constraint-filter-kind"); section.append(node("h4", "", t(kind === "module" ? "web.module" : "web.core")));
    const definitions = [["qualities", "web.filter_quality"], ...(kind === "core" ? [["main_blacklist", "web.constraints_main_blacklist"]] : []), ["sub_blacklist", "web.filter_excluded_sub"]];
    for (const [field, key] of definitions) {
      const label = node("label", "page-field"), select = node("select"); select.multiple = true; select.size = 4;
      select.onchange = () => { filter(kind)[field] = [...select.selectedOptions].map((option) => option.value); changed(); };
      controls.set(`${kind}.${field}`, select); label.append(node("span", "", t(key)), select); section.append(label);
    }
    const label = node("label", "page-field"), mode = node("select");
    for (const [value, key] of [["exclude", "web.constraints_hard_blacklist"], ["zero_weight", "web.constraints_zero_weight"]]) { const option = node("option", "", t(key)); option.value = value; mode.append(option); }
    mode.onchange = () => { filter(kind).blacklist_mode = mode.value; changed(); }; controls.set(`${kind}.blacklist_mode`, mode);
    label.append(node("span", "", t("web.constraints_blacklist_mode")), mode); section.append(label); filters.append(section);
  }
  const keepLabel = node("label", "optimizer-check"), keep = node("input"); keep.type = "checkbox";
  keep.onchange = () => { preferences().keep_current = keep.checked; changed(); }; keepLabel.append(keep, node("span", "", t("web.constraints_keep_current")));
  const missingLabel = node("label", "optimizer-check"), missing = node("input"); missing.type = "checkbox";
  missing.onchange = () => { preferences().allow_missing_core = missing.checked; changed(); }; missingLabel.append(missing, node("span", "", t("web.constraints_missing_core")));
  const limitsHeading = node("h4", "", t("web.constraints_property_limits")), limits = node("div", "property-limits"), add = node("button", "button quiet", t("web.constraints_add_limit")); add.type = "button";
  add.onclick = () => { const first = stats().keys().next().value; if (!first) return; preferences().property_limits ||= []; preferences().property_limits.push({ id: first }); renderLimits(); changed(); };
  content.append(node("p", "optimizer-note", t("web.constraints_hint")), filters, keepLabel, missingLabel, limitsHeading, node("p", "meta", t("web.constraints_limits_hint")), limits, add); details.append(content); mount.append(details);
  function filter(kind) { const value = preferences(); value.filters ||= {}; value.filters[kind] ||= {}; return value.filters[kind]; }
  function renderLimits() {
    const catalog = stats(); limits.replaceChildren();
    for (const [index, entry] of (preferences().property_limits || []).entries()) {
      const row = node("div", "property-limit-row"), property = node("select"); property.setAttribute("aria-label", t("web.optimize_property"));
      if (!catalog.has(entry.id)) catalog.set(entry.id, { id: entry.id, name: propertyLabel(t, entry.id), percent: false });
      for (const stat of catalog.values()) { const option = node("option", "", stat.name); option.value = stat.id; property.append(option); } property.value = entry.id;
      property.onchange = () => { entry.id = property.value; delete entry.minimum; delete entry.maximum; renderLimits(); changed(); };
      row.append(property);
      const percent = catalog.get(entry.id)?.percent === true;
      for (const [field, key] of [["minimum", "web.constraints_minimum"], ["maximum", "web.constraints_maximum"]]) {
        const label = node("label", "page-field"), input = node("input"); input.type = "number"; input.step = "any";
        input.value = entry[field] == null ? "" : String(percent ? entry[field] * 100 : entry[field]);
        input.oninput = () => { if (input.value === "") { delete entry[field]; changed(); } else if (input.checkValidity()) { entry[field] = input.valueAsNumber / (percent ? 100 : 1); changed(); } };
        label.append(node("span", "", t(percent ? `${key}_percent` : key)), input); row.append(label);
      }
      const remove = node("button", "button quiet danger", t("web.remove")); remove.type = "button"; remove.onclick = () => { preferences().property_limits.splice(index, 1); renderLimits(); changed(); };
      row.append(remove); limits.append(row);
    }
  }
  function sync() {
    const catalog = stats(), qualities = new Set((inventory()?.items || []).map((item) => item.quality).filter(Boolean));
    const next = JSON.stringify([[...catalog.values()], [...qualities], preferences().filters || {}, preferences().property_limits || []]);
    if (signature !== next && !details.contains(document.activeElement)) {
      signature = next;
      for (const [key, select] of controls) {
        const [kind, field] = key.split("."), configured = preferences().filters?.[kind] || {};
        if (field === "blacklist_mode") { select.value = configured.blacklist_mode || "exclude"; continue; }
        const selected = configured[field] || [], values = field === "qualities" ? [...new Set([...qualities, ...selected])].map((id) => [id, qualityLabel(t, id)]) : [...catalog.values()].map((stat) => [stat.id, stat.name]);
        for (const id of selected) if (!values.some(([value]) => value === id)) values.push([id, propertyLabel(t, id)]);
        select.replaceChildren(...values.map(([id, name]) => { const option = node("option", "", name); option.value = id; option.selected = selected.includes(id); return option; }));
      }
      renderLimits();
    }
    keep.checked = preferences().keep_current === true; missing.checked = preferences().allow_missing_core === true;
  }
  return {
    render(disabled, software = false) {
      sync(); for (const control of details.querySelectorAll("input,select,button")) control.disabled = disabled;
      keep.disabled = disabled || software; if (software) keep.checked = false;
    },
    request() {
      const value = preferences(), result = {};
      if (value.filters) result.filters = value.filters;
      if (value.property_limits?.length) result.property_limits = value.property_limits;
      if (value.allow_missing_core === true) result.allow_missing_core = true;
      return result;
    },
    reload() { signature = ""; sync(); },
  };
}
