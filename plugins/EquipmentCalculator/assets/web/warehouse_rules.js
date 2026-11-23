import { createRuleEditor, ruleRequest } from "./rule_editor.js";
import { observedEquipmentCatalog } from "./equipment_catalog.js";
import { propertyLabel } from "./equipment_display.js";

const keyOf = (uid) => uid ? `${uid.slot}:${uid.serial}` : "";
export function createWarehouseRules({ mount, api, t, node, inventory, workspace, jobController, canApply, readPreference, savePreference, refreshInventory, openActivity, cancelOperations }) {
  let preferences = { rules: [], include_equipped: false, strict_compatibility: false, protected_shape_ids: [], protected_suit_ids: [], result_page: 1 }, loaded = false, applying = false, appliedPreview = "", error = "", message = "", catalogSignature = "", renderedOutputKey = "";
  const heading = node("div", "page-heading"); heading.append(node("div")); heading.firstChild.append(node("h2", "", t("web.rules_title")), node("p", "meta", t("web.rules_hint")));
  const phase = node("span", "badge"); heading.append(phase);
  const form = node("form", "warehouse-rules-form"), options = node("div", "rule-global-options"), list = node("div", "warehouse-rule-list");
  const fields = {};
  for (const [field, key] of [["include_equipped", "web.rules_include_equipped"], ["strict_compatibility", "web.rules_strict_compatibility"]]) {
    const label = node("label", "optimizer-check"), input = node("input"); input.type = "checkbox"; input.onchange = () => { preferences[field] = input.checked; persist(); }; fields[field] = input; label.append(input, node("span", "", t(key))); options.append(label);
  }
  for (const [field, key] of [["protected_shape_ids", "web.rules_protected_shapes"], ["protected_suit_ids", "web.rules_protected_suits"]]) {
    const label = node("label", "page-field"), input = node("select"); input.multiple = true; input.size = 4; input.onchange = () => { preferences[field] = [...input.selectedOptions].map((option) => option.value); persist(); }; fields[field] = input; label.append(node("span", "", t(key)), input); options.append(label);
  }
  const add = node("button", "button secondary", t("web.rules_add")); add.type = "button"; add.onclick = () => { if (preferences.rules.length >= 256) return; preferences.rules.push({ id: `rule-${crypto.randomUUID()}`, enabled: true, action: "lock", shape_filter: false, suit_filter: false, editor_open: true }); renderEditors(); persist(); };
  const run = node("button", "button primary", t("web.rules_preview")), cancel = node("button", "button quiet", t("web.optimize_cancel")); run.type = "submit"; cancel.type = "button"; cancel.onclick = () => jobController.cancel(); const actions = node("div", "page-actions"); actions.append(add, run, cancel);
  form.append(options, list, actions); const notice = node("p", "tool-message"); notice.setAttribute("role", "status"); const output = node("div", "warehouse-rules-result"); mount.append(heading, form, notice, output);
  function persist() { error = ""; savePreference(JSON.stringify(preferences)); render(); }
  function initialize() {
    try {
      const saved = JSON.parse(readPreference() || "{}");
      if (!saved || typeof saved !== "object" || Array.isArray(saved) || (Object.hasOwn(saved, "rules") && (!Array.isArray(saved.rules) || saved.rules.length > 256 || saved.rules.some((rule) => !rule || typeof rule.id !== "string"))) || ["protected_shape_ids", "protected_suit_ids"].some((key) => Object.hasOwn(saved, key) && (!Array.isArray(saved[key]) || saved[key].some((id) => typeof id !== "string")))) throw new Error();
      preferences = { ...preferences, ...saved };
    } catch { error = t("web.rules_invalid_preferences"); }
    fields.include_equipped.checked = preferences.include_equipped === true; fields.strict_compatibility.checked = preferences.strict_compatibility === true; loaded = true;
  }
  function catalog() {
    const data = observedEquipmentCatalog(inventory()), mains = new Map(), subs = new Map(), qualities = new Set();
    for (const item of inventory()?.items || []) {
      if (item.quality) qualities.add(item.quality); if (item.main_stat?.id) mains.set(item.main_stat.id, propertyLabel(t, item.main_stat));
      for (const stat of item.sub_stats || []) subs.set(stat.id, propertyLabel(t, stat));
    }
    return { ...data, qualities: [...qualities].sort(), mains: [...mains], subs: [...subs], roles: (workspace()?.roles || []).filter((role) => !role.archived) };
  }
  function renderEditors() {
    const data = catalog(); list.replaceChildren(...preferences.rules.map((rule, index) => createRuleEditor({ rule, index, count: preferences.rules.length, catalog: data, t, node, changed: persist,
      remove: () => { preferences.rules.splice(index, 1); renderEditors(); persist(); },
      move: (offset) => { const next = index + offset; [preferences.rules[index], preferences.rules[next]] = [preferences.rules[next], preferences.rules[index]]; renderEditors(); persist(); },
    })));
    if (!preferences.rules.length) list.append(node("p", "empty", t("web.rules_empty")));
    for (const [field, values] of [["protected_shape_ids", data.shapes.map((shape) => [shape.shape_id, shape.shape_id])], ["protected_suit_ids", data.suits.map((suit) => [suit.id, suit.name])]]) {
      const entries = [...values]; for (const id of preferences[field]) if (!entries.some(([value]) => value === id)) entries.push([id, id]);
      fields[field].replaceChildren(...entries.map(([id, name]) => { const option = node("option", "", name); option.value = id; option.selected = preferences[field].includes(id); return option; }));
    }
  }
  function current() { const { job } = jobController.state(); return job.kind === "warehouse_rules" && job.status === "completed" && !job.stale && job.result?.source_snapshot_id === inventory()?.snapshot_id && canApply(); }
  function renderResults() {
    const { job } = jobController.state(), result = job.result;
    const key = `${job.id || ""}|${job.status}|${job.stale}|${current()}|${preferences.result_page}|${applying}|${appliedPreview}`; if (key === renderedOutputKey) return; renderedOutputKey = key;
    output.replaceChildren(); if (job.status !== "completed" || job.kind !== "warehouse_rules" || !result) return;
    const counts = { lock: 0, discard: 0, normal: 0 }; for (const change of result.changes) counts[change.state]++;
    output.append(node("h3", "", t("web.rules_summary", counts.lock, counts.discard, counts.normal)), node("p", "meta mono", t("web.snapshot", result.source_snapshot_id)));
    if (!result.complete) output.append(node("p", "draft-warning", t("web.rules_incomplete")));
    if (job.stale) output.append(node("p", "draft-warning", t("web.rules_stale")));
    const skipped = node("dl", "rule-skips"); for (const [reason, count] of Object.entries(result.skipped || {})) skipped.append(node("dt", "", t(`web.rules_skipped_${reason}`)), node("dd", "", count)); output.append(skipped);
    const actions = node("div", "page-actions"), apply = node("button", "button primary", t("web.rules_apply")); apply.type = "button"; apply.disabled = applying || !current() || !result.changes.length || appliedPreview === result.preview_id;
    apply.onclick = async () => {
      if (applying || !current() || appliedPreview === result.preview_id) return; applying = true; error = ""; message = ""; render();
      try { await api("api/warehouse/rules/apply", { preview_id: result.preview_id, snapshot_id: result.source_snapshot_id }); appliedPreview = result.preview_id; message = t("web.accepted"); await refreshInventory(); }
      catch (failure) { error = failure.message; }
      finally { applying = false; render(); }
    };
    const activity = node("button", "button quiet", t("web.rules_activity")); activity.type = "button"; activity.onclick = openActivity;
    const cancel = node("button", "button quiet", t("web.cancel_pending")); cancel.type = "button"; cancel.onclick = cancelOperations;
    actions.append(apply, activity, cancel); output.append(actions);
    const pages = Math.max(1, Math.ceil(result.changes.length / 100)), page = Math.max(1, Math.min(pages, Number(preferences.result_page) || 1)), paging = node("div", "result-paging");
    for (const [label, value, disabled] of [["web.previous_page", page - 1, page === 1], ["web.next_page", page + 1, page === pages]]) { const button = node("button", "button quiet", t(label)); button.type = "button"; button.disabled = disabled; button.onclick = () => { preferences.result_page = value; persist(); }; paging.append(button); } paging.append(node("span", "meta", t("web.inventory_page", page, pages))); output.append(paging);
    const table = node("table", "data-table"), head = node("thead"), header = node("tr"); for (const key of ["web.inventory", "web.rules_previous_state", "web.rules_target_state", "web.rules_name", "web.optimize_score", "web.optimize_grade"]) header.append(node("th", "", t(key))); head.append(header); table.append(head); const body = node("tbody");
    for (const change of result.changes.slice((page - 1) * 100, page * 100)) {
      const item = inventory()?.items?.find((item) => keyOf(item.uid) === keyOf(change.equipment)), rule = result.rules_snapshot?.find((rule) => rule.id === change.rule_id), row = node("tr");
      const stateKey = (state) => state === "lock" ? "web.locked" : state === "discard" ? "web.discarded" : "web.normal";
      row.append(node("th", "", item?.name || keyOf(change.equipment)), node("td", "", t(stateKey(change.previous_state))), node("td", "", t(stateKey(change.state))), node("td", "", rule?.name || change.rule_id), node("td", "", change.score ?? "—"), node("td", "", change.grade ?? "—")); body.append(row);
    }
    table.append(body); output.append(table);
  }
  function render() {
    const { job, active, posting, error: jobError } = jobController.state(), disabled = !loaded || applying || active || posting;
    for (const control of form.querySelectorAll("input,select,button")) control.disabled = disabled || control.dataset.orderDisabled === "true" || control.dataset.filterDisabled === "true" || (control.closest(".warehouse-rule")?.dataset.disabledRule === "true" && Boolean(control.closest(".rule-filters")));
    run.disabled = disabled || !canApply() || !preferences.rules.length; add.disabled = disabled || preferences.rules.length >= 256; cancel.disabled = !active || posting;
    phase.textContent = t(`web.optimize_status_${job.status || "idle"}`); notice.textContent = error || jobError || message || (job.kind === "warehouse_rules" && job.status === "failed" ? job.message : "") || ""; notice.hidden = !notice.textContent; notice.classList.toggle("error", Boolean(error || jobError) || job.status === "failed"); renderResults();
  }
  form.onsubmit = async (event) => {
    event.preventDefault(); if (!canApply() || !form.reportValidity() || !preferences.rules.length) return; error = ""; message = "";
    await jobController.run("api/warehouse/rules/preview", { snapshot_id: inventory().snapshot_id, rules: preferences.rules.map(ruleRequest), include_equipped: preferences.include_equipped,
      strict_compatibility: preferences.strict_compatibility, protected_shape_ids: preferences.protected_shape_ids, protected_suit_ids: preferences.protected_suit_ids });
  };
  jobController.subscribe(render);
  return { activate() { if (!loaded) initialize(); render(); }, sync() { if (!workspace()?.account) return; if (!loaded) initialize(); const signature = JSON.stringify(catalog()); if (signature !== catalogSignature && !form.contains(document.activeElement)) { catalogSignature = signature; renderEditors(); } render(); } };
}
