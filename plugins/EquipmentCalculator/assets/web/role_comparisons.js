import { equipmentIcon } from "./asset_images.js";
const keyOf = (uid) => uid ? `${uid.slot}:${uid.serial}` : "";
export function createRoleComparisons({ controlsMount, resultMount, t, node, inventory, workspace,
  selectedRole, selectedSlot, preferences, persist, features, jobController, inspectItem }) {
  const modeLabel = node("label", "page-field comparison-mode"), mode = node("select");
  for (const [value, key] of [["panel", "web.panel_mode_attributes"], ["contributions", "web.panel_mode_contributions"], ["replacements", "web.panel_mode_replacements"]]) { const option = node("option", "", t(key)); option.value = value; mode.append(option); }
  modeLabel.append(node("span", "", t("web.panel_calculation_mode")), mode); mode.onchange = () => { preferences().calculation_kind = mode.value; persist(); };
  const replacements = node("div", "replacement-options"), targetLabel = node("label", "page-field"), target = node("select");
  targetLabel.append(node("span", "", t("web.comparison_target")), target); target.onchange = () => { preferences().replacement_equipment = target.value; persist(); };
  const limitLabel = node("label", "page-field"), limit = node("input"); limit.type = "number"; limit.min = "1"; limit.max = "500"; limit.step = "1";
  limitLabel.append(node("span", "", t("web.comparison_limit")), limit); limit.oninput = () => { if (limit.checkValidity()) { preferences().replacement_limit = limit.valueAsNumber; persist(); } };
  replacements.append(targetLabel, limitLabel);
  const toggles = {};
  for (const [field, key] of [["replacement_allow_borrow", "web.optimize_allow_borrow"], ["replacement_include_discarded", "web.optimize_include_discarded"]]) {
    const label = node("label", "optimizer-check"), input = node("input"); input.type = "checkbox"; input.onchange = () => { preferences()[field] = input.checked; persist(); }; label.append(input, node("span", "", t(key))); replacements.append(label); toggles[field] = input;
  }
  controlsMount.prepend(modeLabel); controlsMount.append(replacements);
  const output = node("section", "comparison-results"); resultMount.append(output);
  let targets = [], targetSignature = "", renderedKey = "";
  const enabled = () => features().role_comparisons === true;
  const kind = () => enabled() && ["contributions", "replacements"].includes(preferences().calculation_kind) ? preferences().calculation_kind : "panel";
  const findItem = (uid) => inventory()?.items?.find((item) => keyOf(item.uid) === keyOf(uid));
  const itemName = (uid) => { const item = findItem(uid); return item?.name || item?.item_id || keyOf(uid); };
  const number = (value) => value == null ? "—" : new Intl.NumberFormat(document.documentElement.lang || "en", { maximumFractionDigits: 2 }).format(value);
  const gain = (value) => value == null ? "—" : t("web.panel_percent_value", number(value));
  function updateTargets() {
    const role = selectedRole(), source = selectedSlot();
    if (source) {
      const plan = workspace()?.slots?.find((slot) => slot.id === source && slot.role_id === role?.id)?.plan;
      const uids = plan ? [...(plan.core ? [plan.core] : []), ...plan.placements.map((placement) => placement.equipment)] : [];
      targets = uids.map(findItem).filter(Boolean);
    } else targets = (inventory()?.items || []).filter((item) => role?.character && keyOf(item.character) === keyOf(role.character));
    const values = [["", t("web.comparison_choose_target")], ...targets.map((item) => [keyOf(item.uid), item.name || item.item_id || keyOf(item.uid)])], signature = JSON.stringify(values);
    if (signature !== targetSignature) { targetSignature = signature; target.replaceChildren(...values.map(([value, label]) => { const option = node("option", "", label); option.value = value; return option; })); }
    target.value = preferences().replacement_equipment || "";
  }
  function inspectButton(uid, roleId) {
    const button = node("button", "button quiet", t("web.comparison_inspect")); button.type = "button"; button.disabled = !findItem(uid);
    button.onclick = () => { if (findItem(uid)) inspectItem(uid, roleId); }; return button;
  }
  function renderResults() {
    const { job } = jobController.state(), result = job.result;
    const key = `${job.id || ""}|${job.status}|${job.stale}|${job.profile_stale}|${inventory()?.snapshot_id}|${preferences().replacement_result_page}`; if (key === renderedKey) return; renderedKey = key;
    output.replaceChildren(); output.hidden = job.status !== "completed" || !["contributions", "replacements"].includes(job.kind) || !result;
    if (output.hidden) return;
    output.append(node("h3", "", t(job.kind === "contributions" ? "web.panel_mode_contributions" : "web.panel_mode_replacements")), node("p", "meta", t("web.comparison_frozen")));
    if (result.role_id) output.append(node("p", "meta", t("web.panel_result_for", workspace()?.roles?.find((role) => role.id === result.role_id)?.name || result.role_id)));
    if (job.stale) output.append(node("p", "draft-warning", t("web.optimize_stale_preview")));
    if (job.profile_stale) output.append(node("p", "draft-warning", t("web.panel_profile_stale")));
    if (result.complete === false || result.available === false) output.append(node("p", "draft-warning", t("web.panel_incomplete")));
    if (result.equipment_projection === "maximum_level") output.append(node("p", "meta", t("web.panel_max_projection")));
    if (job.kind === "contributions" && result.equipment_gain) {
      const summary = node("div", "optimization-metrics");
      for (const [key, value] of [["web.comparison_equipment_gain", result.equipment_gain.available ? gain(result.equipment_gain.gain_percent) : "—"], ["web.comparison_damage_with", number(result.equipment_gain.damage)], ["web.comparison_damage_without", number(result.equipment_gain.baseline_damage)]]) { const metric = node("div"); metric.append(node("span", "field-label", t(key)), node("strong", "", value)); summary.append(metric); } output.append(summary);
      const table = node("table", "data-table"), head = node("thead"), header = node("tr"); for (const key of ["web.inventory", "web.comparison_damage_with", "web.comparison_damage_without", "web.panel_gain"]) header.append(node("th", "", t(key))); head.append(header); table.append(head); const body = node("tbody");
      for (const item of result.items || []) { const row = node("tr"), name = node("th"); name.append(equipmentIcon(findItem(item.equipment), node, "small"), node("span", "", itemName(item.equipment)), inspectButton(item.equipment, result.role_id)); row.append(name, node("td", "", number(item.damage)), node("td", "", number(item.baseline_damage)), node("td", "", item.available ? gain(item.gain_percent) : "—")); body.append(row); } table.append(body); output.append(table);
    }
    if (job.kind === "replacements" && result.equipment) {
      output.append(node("p", "comparison-target", t("web.comparison_for", itemName(result.equipment))), node("p", "meta", t("web.comparison_ranking_hint")), node("p", "meta", t("web.comparison_current_scores", number(result.current_score), number(result.current_direct_damage_score))), node("p", "meta", t("web.comparison_returned", result.candidates?.length ?? 0, result.total ?? "—")));
      if (result.truncated) output.append(node("p", "draft-warning", t("web.comparison_truncated")));
      const pages = Math.max(1, Math.ceil((result.candidates?.length || 0) / 50)), page = Math.max(1, Math.min(pages, Number(preferences().replacement_result_page) || 1)), paging = node("div", "result-paging");
      for (const [key, value, disabled] of [["web.previous_page", page - 1, page === 1], ["web.next_page", page + 1, page === pages]]) { const button = node("button", "button quiet", t(key)); button.type = "button"; button.disabled = disabled; button.onclick = () => { preferences().replacement_result_page = value; persist(); }; paging.append(button); } paging.append(node("span", "meta", t("web.inventory_page", page, pages))); output.append(paging);
      const table = node("table", "data-table"), head = node("thead"), header = node("tr"); for (const key of ["web.inventory", "web.optimize_score", "web.optimize_grade", "web.comparison_damage", "web.panel_gain", "web.comparison_direct_score"]) header.append(node("th", "", t(key))); head.append(header); table.append(head); const body = node("tbody");
      for (const candidate of (result.candidates || []).slice((page - 1) * 50, page * 50)) { const row = node("tr"), name = node("th"); name.append(equipmentIcon(findItem(candidate.equipment), node, "small"), node("span", "", itemName(candidate.equipment)), inspectButton(candidate.equipment, result.role_id)); row.append(name, node("td", "", number(candidate.score)), node("td", "", candidate.grade ?? "—"), node("td", "", number(candidate.damage)), node("td", "", gain(candidate.gain_percent)), node("td", "", number(candidate.direct_damage_score))); body.append(row); } table.append(body); output.append(table);
      if (result.errors?.length) {
        const errors = node("details", "panel-source"); errors.append(node("summary", "", t("web.comparison_errors", result.errors.length)));
        for (const failure of result.errors) { const translated = t(failure.error_code); errors.append(node("p", "meta", t("web.comparison_item_error", itemName(failure.equipment), translated === failure.error_code ? t("web.comparison_candidate_unavailable") : translated))); } output.append(errors);
      }
    }
    const source = node("details", "panel-source"); source.append(node("summary", "", t("web.panel_profile_source")), node("pre", "mono", JSON.stringify({ role_id: result.role_id, profile: result.profile, missing: result.missing, sources: result.sources }, null, 2))); output.append(source);
  }
  return {
    kind,
    render(disabled) {
      modeLabel.hidden = !enabled(); mode.value = kind(); mode.disabled = disabled;
      replacements.hidden = kind() !== "replacements"; updateTargets();
      target.required = limit.required = kind() === "replacements"; target.disabled = limit.disabled = disabled || kind() !== "replacements";
      if (document.activeElement !== limit) limit.value = preferences().replacement_limit ?? 200;
      for (const [field, input] of Object.entries(toggles)) { input.disabled = disabled || kind() !== "replacements"; input.checked = field === "replacement_allow_borrow" ? preferences()[field] !== false : preferences()[field] === true; }
      renderResults();
    },
    extraRequest() {
      if (kind() !== "replacements") return {};
      const selected = targets.find((item) => keyOf(item.uid) === target.value);
      return { equipment: selected.uid, snapshot_id: inventory().snapshot_id, limit: limit.valueAsNumber, allow_borrow: preferences().replacement_allow_borrow !== false, include_discarded: preferences().replacement_include_discarded === true };
    },
  };
}
