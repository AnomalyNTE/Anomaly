import { observedEquipmentCatalog, shapeThumbnail } from "./equipment_catalog.js";

const keyOf = (uid) => uid ? `${uid.slot}:${uid.serial}` : "";
const defaults = { mode: "demand", strategy: "balanced", target_grade: "S", target_percent: "", roles: {}, demand: {}, suit_id: "" };
export function createRewindPanel({ mount, api, t, node, workspace, inventory, isReady, readPreference, savePreference }) {
  let preferences = structuredClone(defaults), loaded = false, busy = false, error = "", result = null, source = [], signature = "", resultSnapshot = "";
  const heading = node("div", "page-heading"); heading.append(node("div")); heading.firstChild.append(node("h2", "", t("web.rewind_title")), node("p", "meta", t("web.rewind_hint")));
  const form = node("form", "tool-form"), options = node("div", "tool-options"), fields = {};
  for (const [field, key, values] of [
    ["mode", "web.rewind_mode", [["demand", "web.rewind_demand"], ["types", "web.rewind_types"], ["score_shortfall", "web.rewind_shortfall"]]],
    ["strategy", "web.rewind_strategy", [["balanced", "web.rewind_balanced"], ["focus", "web.rewind_focus"]]],
    ["target_grade", "web.rewind_target_grade", ["D", "C", "B", "A", "S", "SS", "SSS", "ACE"].map((grade) => [grade, null])],
  ]) {
    const label = node("label", "page-field"), select = node("select");
    for (const [value, caption] of values) { const option = node("option", "", caption ? t(caption) : value); option.value = value; select.append(option); }
    select.onchange = () => { preferences[field] = select.value; persist(); }; fields[field] = select; label.append(node("span", "", t(key)), select); options.append(label);
  }
  const percentLabel = node("label", "page-field"), percent = node("input"); percent.type = "number"; percent.min = "1"; percent.max = "100"; percent.step = "any";
  percent.oninput = () => { if (percent.checkValidity()) { preferences.target_percent = percent.value; persist(); } }; fields.target_percent = percent;
  percentLabel.append(node("span", "", t("web.rewind_target_percent")), percent, node("small", "meta", t("web.rewind_percent_hint"))); options.append(percentLabel);
  const sourceRoles = node("div", "rewind-role-sources"), demand = node("div", "rewind-demand"), suitLabel = node("label", "page-field"), suitSelect = node("select"), fill = node("button", "button quiet", t("web.rewind_fill_suit"));
  suitLabel.append(node("span", "", t("web.filter_suit")), suitSelect); fill.type = "button";
  suitSelect.onchange = () => { preferences.suit_id = suitSelect.value; persist(); };
  fill.onclick = () => {
    const suit = observedEquipmentCatalog(inventory()).suits.find((suit) => suit.id === suitSelect.value); if (!suit) return;
    preferences.demand = {}; for (const shape of suit.required_shapes) preferences.demand[shape] = (preferences.demand[shape] || 0) + 1;
    signature = ""; persist(); catalog(); render();
  };
  const demandHeader = node("div", "rewind-demand-header"); demandHeader.append(suitLabel, fill);
  const demandSection = node("section", "rewind-demand-section"); demandSection.append(demandHeader, node("p", "meta", t("web.rewind_demand_hint")), demand);
  const run = node("button", "button primary", t("web.rewind_run")); run.type = "submit";
  const actions = node("div", "page-actions"); actions.append(run); form.append(options, demandSection, sourceRoles, actions);
  const notice = node("p", "page-error"); notice.setAttribute("role", "status"); const output = node("div", "rewind-results"); mount.append(heading, form, notice, output);
  function persist() { error = ""; result = null; source = []; savePreference(JSON.stringify(preferences)); render(); }
  function initialize() {
    try {
      const saved = JSON.parse(readPreference() || "{}");
      if (!saved || typeof saved !== "object" || Array.isArray(saved) || ["roles", "demand"].some((key) => Object.hasOwn(saved, key) && (!saved[key] || typeof saved[key] !== "object" || Array.isArray(saved[key])))) throw new Error();
      preferences = { ...preferences, ...saved };
    } catch { error = t("web.rewind_invalid_preferences"); }
    for (const [field, input] of Object.entries(fields)) input.value = preferences[field]; loaded = true;
  }
  function catalog() {
    const data = observedEquipmentCatalog(inventory()), roles = (workspace()?.roles || []).filter((role) => !role.archived), slots = (workspace()?.slots || []).filter((slot) => !slot.archived && slot.plan);
    const next = JSON.stringify([data, roles.map((role) => [role.id, role.name]), slots.map((slot) => [slot.id, slot.role_id, slot.name, slot.current_revision])]);
    if (signature === next) return; signature = next;
    suitSelect.replaceChildren(...[["", t("web.optimize_any_suit")], ...data.suits.map((suit) => [suit.id, suit.name])].map(([value, caption]) => { const option = node("option", "", caption); option.value = value; return option; })); suitSelect.value = preferences.suit_id;
    demand.replaceChildren();
    for (const shape of data.shapes) {
      const label = node("label", "rewind-demand-row"), input = node("input"); input.type = "number"; input.min = "0"; input.max = "1000"; input.step = "1"; input.required = true; input.value = preferences.demand[shape.shape_id] || 0;
      input.setAttribute("aria-label", t("web.rewind_demand_for", shape.shape_id)); input.oninput = () => { if (input.checkValidity()) { preferences.demand[shape.shape_id] = input.valueAsNumber; persist(); } };
      label.append(shapeThumbnail(shape, node), node("span", "mono", shape.shape_id), node("span", "meta", t("web.rewind_owned", data.counts[shape.shape_id])), input); demand.append(label);
    }
    sourceRoles.replaceChildren(node("h3", "", t("web.rewind_saved_sources")), node("p", "meta", t("web.rewind_saved_hint")));
    for (const role of roles) {
      const available = slots.filter((slot) => slot.role_id === role.id); if (!available.length) continue;
      const row = node("div", "rewind-source-row"), label = node("label", "page-field"), select = node("select");
      const choice = preferences.roles[role.id] || { slot_id: "", main: false };
      for (const [value, caption] of [["", t("web.rewind_ignore_role")], ...available.map((slot) => [slot.id, slot.name || t("web.library_primary")])]) { const option = node("option", "", caption); option.value = value; select.append(option); } select.value = choice.slot_id;
      label.append(node("span", "", role.name), select);
      const mainLabel = node("label", "optimizer-check"), main = node("input"); main.type = "checkbox"; main.checked = choice.main === true;
      mainLabel.append(main, node("span", "", t("web.rewind_main_role")));
      const changed = () => { preferences.roles[role.id] = { slot_id: select.value, main: main.checked }; persist(); };
      select.onchange = changed; main.onchange = changed; row.append(label, mainLabel); sourceRoles.append(row);
    }
  }
  function render() {
    const shortfall = preferences.mode === "score_shortfall";
    for (const control of form.querySelectorAll("input,select,button")) control.disabled = busy || !loaded;
    for (const field of ["target_grade", "target_percent"]) fields[field].disabled = busy || !shortfall;
    fields.target_grade.disabled ||= percent.value !== "";
    sourceRoles.hidden = !shortfall; demandSection.hidden = shortfall;
    for (const control of sourceRoles.querySelectorAll("input,select")) control.disabled = busy || !shortfall;
    for (const control of demandSection.querySelectorAll("input,select,button")) control.disabled = busy || shortfall;
    run.disabled = busy || !loaded || !isReady(); run.textContent = t(busy ? "web.rewind_running" : "web.rewind_run");
    notice.textContent = error; notice.hidden = !error;
    output.replaceChildren(); if (!result) return;
    if (resultSnapshot !== inventory()?.snapshot_id) output.append(node("p", "draft-warning", t("web.rewind_stale")));
    if (!result.valid) { output.append(node("p", "draft-warning", t(result.reason === "no_shortfall" ? "web.rewind_no_shortfall" : result.reason === "too_many_shapes" ? "web.rewind_too_many_shapes" : "web.rewind_no_result"))); return; }
    const summary = node("div", "rewind-summary"); summary.append(node("strong", "", t("web.rewind_total", result.slots, result.total_cost))); output.append(summary);
    if (source.length) output.append(node("p", "meta", t("web.rewind_source_summary", source.join(" · "))));
    const table = node("table", "data-table"), head = node("thead"), header = node("tr");
    for (const key of ["web.filter_shape", "web.rewind_quantity", "web.rewind_probability", "web.rewind_unit_cost", "web.rewind_cost"]) header.append(node("th", "", t(key))); head.append(header); table.append(head);
    const body = node("tbody");
    for (const recommendation of result.recommendations) {
      const row = node("tr"); row.append(node("th", "mono", recommendation.shape_id), node("td", "", recommendation.quantity), node("td", "", new Intl.NumberFormat(document.documentElement.lang || "en", { style: "percent", maximumFractionDigits: 1 }).format(recommendation.probability)), node("td", "", recommendation.unit_cost), node("td", "", recommendation.cost)); body.append(row);
    }
    table.append(body); output.append(table, node("p", "meta", t("web.rewind_no_consumption")));
  }
  form.onsubmit = async (event) => {
    event.preventDefault(); if (busy || !isReady() || !form.reportValidity()) return;
    busy = true; error = ""; render();
    try {
      const data = observedEquipmentCatalog(inventory()), definitions = new Map(data.shapes.map((shape) => [shape.shape_id, { shape_id: shape.shape_id, area: shape.cells.length }]));
      const sourceSnapshot = inventory().snapshot_id;
      const request = { mode: preferences.mode, strategy: preferences.strategy, owned_counts: data.counts, target_grade: preferences.target_grade };
      const sources = [];
      if (preferences.target_percent !== "") request.target_percent = Number(preferences.target_percent) / 100;
      if (preferences.mode === "score_shortfall") {
        request.deficits = [];
        for (const [roleId, choice] of Object.entries(preferences.roles)) {
          if (!choice.slot_id) continue;
          const frozen = await api(`api/slots/${encodeURIComponent(roleId)}/${encodeURIComponent(choice.slot_id)}`);
          if (!Array.isArray(frozen.assignment_scores)) throw new Error(t("web.rewind_missing_scores"));
          for (const placement of frozen.slot.plan.placements) {
            const item = frozen.items.find((item) => keyOf(item.uid) === keyOf(placement.equipment)), score = frozen.assignment_scores.find((score) => keyOf(score.equipment) === keyOf(placement.equipment));
            if (!item?.shape_id || !Number.isFinite(score?.score) || !Number.isInteger(score.area)) throw new Error(t("web.rewind_missing_scores"));
            definitions.set(item.shape_id, { shape_id: item.shape_id, area: score.area });
            request.deficits.push({ shape_id: item.shape_id, area: score.area, score: score.score, main: choice.main });
          }
          sources.push(`${frozen.role.name} · ${frozen.slot.name || t("web.library_primary")}`);
        }
        if (!request.deficits.length) throw new Error(t("web.rewind_choose_sources"));
      } else {
        request.requirements = [];
        for (const [shape, count] of Object.entries(preferences.demand)) for (let index = 0; index < count; index++) request.requirements.push(shape);
        if (!request.requirements.length) throw new Error(t("web.rewind_choose_demand"));
      }
      request.shapes = [...definitions.values()]; result = await api("api/rewind/recommend", request); source = sources; resultSnapshot = sourceSnapshot;
    } catch (failure) { error = failure.message; }
    finally { busy = false; render(); }
  };
  return { activate() { if (!loaded) initialize(); catalog(); render(); }, sync() { if (!workspace()?.account) return; if (!loaded) initialize(); if (!busy) catalog(); render(); } };
}
