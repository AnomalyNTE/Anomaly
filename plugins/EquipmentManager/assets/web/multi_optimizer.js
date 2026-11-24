import { renderPlanPreview } from "./optimization_results.js";
import { createLoadoutSave } from "./save_loadouts.js";
import { characterAvatar, updateCharacterAvatar } from "./asset_images.js";
import { resolveOptimizationPreference, optimizationPreferenceFields } from "./weight_preferences.js";
import { createNumberValidation } from "./number_validation.js";

const keyOf = (uid) => uid ? `${uid.slot}:${uid.serial}` : "";
const defaults = { roles: {}, top_k: 5, combination_budget: 5000000, max_nodes: 1000000000, time_limit_ms: 120000, worker_count: 0, include_role_top_k: false, exclude_selected: false };

export function createMultiOptimizer({ mount, api, t, node, workspace, inventory, jobController, canRun,
  readPreference, savePreference, readRolePreferences, openPreference, selectedUids, loadDraft, loadPlans, applyPlans, saveSoftwarePlan, reloadWorkspace, readSaveSlots, saveSaveSlots }) {
  let preferences = structuredClone(defaults), loaded = false, accountId = "", error = "", preferenceError = "", renderedOutputKey = "";
  const roleCards = new Map(), queueRows = new Map();
  const heading = node("div", "page-heading");
  heading.append(node("h2", "", t("web.multi_title")));
  const phase = node("span", "badge"); heading.append(phase);
  const form = node("form", "multi-form"), mainColumn = node("div", "multi-main"), rows = node("div", "multi-roles");
  const validateNumbers = createNumberValidation(form, t, ".multi-options input[type=number]");
  const sidebar = node("aside", "multi-sidebar");
  const queueSection = node("div", "multi-queue-section"), queueHeader = node("div", "multi-queue-header"), queueHint = node("span", "multi-queue-hint", t("web.multi_priority")), queueList = node("div", "multi-queue");
  const count = node("span", "multi-selected-count");
  const picker = node("details", "multi-character-picker"), pickerSummary = node("summary", "multi-character-summary"), selectedNames = node("span", "multi-selected-names");
  picker.open = true; pickerSummary.append(count, selectedNames); picker.append(pickerSummary, rows);
  const clearSelection = node("button", "button quiet multi-clear-selection", t("web.board_cancel_selection")); clearSelection.type = "button";
  clearSelection.onclick = () => {
    for (const role of available()) if (preferences.roles[role.id]?.enabled) preferences.roles[role.id].enabled = false;
    persist();
  };
  queueHeader.append(queueHint, clearSelection);
  const toolbar = node("div", "multi-toolbar"), run = node("button", "button primary", t("web.multi_run"));
  run.type = "submit"; toolbar.append(run);
  queueSection.append(queueHeader, queueList, toolbar);
  const advanced = node("details", "multi-advanced"); advanced.append(node("summary", "", t("web.advanced_options")));
  const options = node("div", "multi-options"), controls = {};
  for (const [field, key, min, max, scale = 1] of [["top_k", "web.multi_candidate_limit", 1, 20], ["combination_budget", "web.multi_combination_budget", 1, 5000000], ["max_nodes", "web.optimize_node_limit", 1, 1000000000], ["time_limit_ms", "web.optimize_time_limit_seconds", 0.001, 120, 1000], ["worker_count", "web.optimize_worker_count", 0, 32]]) {
    const label = node("label", "page-field"), input = node("input"); input.type = "number"; input.min = min; input.max = max; input.step = scale === 1000 ? "0.001" : "1"; input.required = true;
    input.oninput = () => { if (input.checkValidity()) { preferences[field] = Math.round(input.valueAsNumber * scale); persist(); } };
    controls[field] = input; label.append(node("span", "", t(key)), input); options.append(label);
  }
  for (const [field, key] of [["include_role_top_k", "web.multi_include_alternatives"], ["exclude_selected", "web.multi_exclude_selected"]]) {
    const label = node("label", "optimizer-check"), input = node("input"); input.type = "checkbox"; input.onchange = () => { preferences[field] = input.checked; persist(); };
    controls[field] = input; label.append(input, node("span", "", t(key))); options.append(label);
  }
  const increase = node("button", "button quiet", t("web.optimize_increase_budget")); increase.type = "button"; increase.onclick = increaseBudget;
  advanced.append(options, increase);
  mainColumn.append(picker, advanced);
  sidebar.append(queueSection);
  form.append(mainColumn, sidebar);
  const notice = node("p", "tool-message"); notice.setAttribute("role", "status");
  const output = node("div", "multi-results"); mount.append(heading, form, notice, output);
  function available() { return (workspace()?.roles || []).filter((role) => !role.archived); }
  function roleName(plan) { return workspace()?.roles?.find((role) => role.id === plan.role_id)?.name; }
  function identityName(value) { return value?.role_id ? workspace()?.roles?.find((role) => role.id === value.role_id)?.name || value.role_id : inventory()?.characters?.find((character) => keyOf(character.uid) === keyOf(value))?.name || keyOf(value); }
  function persist() { error = ""; savePreference(JSON.stringify(preferences)); render(); }
  function updateRolePreference(roleId, patch, rerender = true) {
    preferences.roles[roleId] = { enabled: false, priority: 1, keep_slot_id: "", ...preferences.roles[roleId], ...patch };
    error = "";
    savePreference(JSON.stringify(preferences));
    if (rerender) render();
  }
  function initialize() {
    preferences = structuredClone(defaults);
    try {
      const saved = JSON.parse(readPreference() || "{}");
      if (!saved || typeof saved !== "object" || Array.isArray(saved) || (Object.hasOwn(saved, "roles") && (!saved.roles || typeof saved.roles !== "object" || Array.isArray(saved.roles))) || (Object.hasOwn(saved, "worker_count") && (!Number.isInteger(saved.worker_count) || saved.worker_count < 0 || saved.worker_count > 32))) throw new Error();
      preferences = { ...preferences, ...saved };
    } catch { error = t("web.multi_invalid_preferences"); }
    for (const [field, input] of Object.entries(controls)) if (input.type === "checkbox") input.checked = preferences[field] === true; else input.value = field === "time_limit_ms" ? preferences[field] / 1000 : preferences[field];
    accountId = workspace()?.account?.id || ""; loaded = true;
  }
  function rolePreferences() {
    const value = JSON.parse(readRolePreferences() || "{}");
    if (!value || typeof value !== "object" || Array.isArray(value)) throw new Error(t("web.optimize_invalid_preferences"));
    return value;
  }
  function unavailable(role) {
    if (!role.character) return role.official === false && role.allowed_cells?.length === 20 ? "" : "web.multi_not_live";
    const character = inventory()?.characters?.find((value) => keyOf(value.uid) === keyOf(role.character));
    if (!character) return "web.multi_not_live";
    return character.allowed_cells?.length === 20 ? "" : "web.multi_board_missing";
  }
  function createRoleCard(role) {
    const card = node("div", "multi-role-card"), choice = node("label", "multi-role-choice"), enabled = node("input"), copy = node("span", "multi-role-copy"), name = node("strong");
    enabled.type = "checkbox";
    const avatar = characterAvatar(role, node), state = node("small", "multi-role-state");
    copy.append(name, state);
    choice.append(avatar, copy, enabled);
    card.append(choice);
    const record = { card, enabled, avatar, name, state, role };
    enabled.onchange = () => updateRolePreference(record.role.id, { enabled: enabled.checked });
    return record;
  }
  function createQueueRow(role) {
    const row = node("div", "multi-queue-row");
    const head = node("div", "multi-queue-head");
    const avatar = characterAvatar(role, node, "small");
    const copy = node("div", "multi-queue-copy");
    const name = node("strong"), prefSource = node("small", "meta");
    copy.append(name, prefSource);
    const priorityWrap = node("label", "multi-queue-priority-wrap");
    const priority = node("input", "multi-queue-priority");
    priority.type = "number"; priority.min = "1"; priority.max = "64"; priority.step = "1"; priority.required = true;
    priorityWrap.append(priority);
    const remove = node("button", "icon-button", "×");
    remove.type = "button";
    head.append(avatar, copy, priorityWrap, remove);
    const footer = node("div", "multi-queue-controls");
    const keep = node("select", "multi-queue-keep");
    const edit = node("button", "button quiet multi-edit-preference", t("web.multi_edit_preference"));
    edit.type = "button";
    footer.append(keep, edit);
    row.append(head, footer);
    const record = { row, avatar, name, prefSource, priorityWrap, priority, remove, keep, edit, role, slotsKey: "" };
    priority.oninput = () => {
      if (!priority.checkValidity()) return;
      const value = priority.valueAsNumber;
      priorityWrap.title = t("web.multi_priority_value", value);
      updateRolePreference(record.role.id, { enabled: true, priority: value }, false);
    };
    priority.onblur = () => render();
    keep.onchange = () => updateRolePreference(record.role.id, { enabled: true, keep_slot_id: keep.value });
    edit.onclick = () => openPreference(record.role);
    remove.onclick = () => updateRolePreference(record.role.id, { enabled: false });
    return record;
  }
  function renderRows() {
    const roles = available();
    let stored; preferenceError = "";
    try { stored = rolePreferences(); } catch { stored = {}; preferenceError = t("web.optimize_invalid_preferences"); }
    if (!roles.length) { rows.replaceChildren(node("p", "empty", t("web.multi_no_roles"))); roleCards.clear(); return stored; }
    const ids = new Set(roles.map((role) => role.id));
    for (const [id, record] of roleCards) if (!ids.has(id)) { record.card.remove(); roleCards.delete(id); }
    if (!roleCards.size) rows.replaceChildren();
    for (const [index, role] of roles.entries()) {
      const preference = preferences.roles[role.id] || { enabled: false, priority: 1, keep_slot_id: "" };
      let record = roleCards.get(role.id);
      if (!record) { record = createRoleCard(role); roleCards.set(role.id, record); }
      record.role = role; record.name.textContent = role.name; updateCharacterAvatar(record.avatar, role);
      record.card.setAttribute("role", "group"); record.card.setAttribute("aria-label", role.name);
      const reason = unavailable(role), selected = preference.enabled === true;
      record.enabled.checked = selected; record.enabled.dataset.unavailable = String(Boolean(reason) && !selected);
      record.card.classList.toggle("selected", selected); record.card.classList.toggle("unavailable", Boolean(reason));
      record.avatar.classList.toggle("selected", selected);
      record.enabled.setAttribute("aria-label", role.name);
      record.state.textContent = reason ? t(reason) : "";
      record.state.hidden = !record.state.textContent;
      record.card.title = reason ? t(reason) : role.name;
      if (rows.children[index] !== record.card) rows.insertBefore(record.card, rows.children[index] || null);
    }
    return stored;
  }
  function renderQueue(selected, stored) {
    const slots = workspace()?.slots || [];
    const ordered = [...selected].sort((left, right) => (preferences.roles[right.id]?.priority ?? 1) - (preferences.roles[left.id]?.priority ?? 1));
    if (!ordered.length) {
      queueRows.clear();
      queueList.replaceChildren(node("p", "empty compact", t("web.choose_character")));
      return;
    }
    const selectedIds = new Set(ordered.map((role) => role.id));
    for (const [id, record] of queueRows) if (!selectedIds.has(id)) { record.row.remove(); queueRows.delete(id); }
    if (!queueRows.size) queueList.replaceChildren();
    const editingPriority = queueList.contains(document.activeElement) && document.activeElement.classList.contains("multi-queue-priority");
    for (const [index, role] of ordered.entries()) {
      const pref = preferences.roles[role.id] || { enabled: true, priority: 1, keep_slot_id: "" };
      let record = queueRows.get(role.id);
      if (!record) { record = createQueueRow(role); queueRows.set(role.id, record); }
      record.role = role;
      record.name.textContent = role.name;
      updateCharacterAvatar(record.avatar, role);
      const resolved = resolveOptimizationPreference(role, stored[role.id]);
      const preset = ["recommended", "general", "role", "custom"].includes(resolved.preset) ? resolved.preset : "custom";
      record.prefSource.textContent = !preferenceError ? t(`web.preference_${preset}`) : "";
      record.prefSource.hidden = !record.prefSource.textContent;
      record.priority.setAttribute("aria-label", t("web.multi_priority_for", role.name));
      record.priorityWrap.title = t("web.multi_priority_value", pref.priority ?? 1);
      if (document.activeElement !== record.priority) record.priority.value = String(pref.priority ?? 1);
      record.keep.setAttribute("aria-label", t("web.multi_keep_for", role.name));
      record.keep.title = t("web.multi_keep");
      const choices = [["", t("web.multi_keep_none")], ...slots.filter((slot) => slot.role_id === role.id && slot.plan && !slot.archived).map((slot) => [slot.id, slot.name || t("web.library_primary")])];
      const slotsKey = JSON.stringify(choices);
      if (record.slotsKey !== slotsKey) {
        record.keep.replaceChildren(...choices.map(([id, label]) => { const option = node("option", "", label); option.value = id; return option; }));
        record.slotsKey = slotsKey;
      }
      record.keep.value = pref.keep_slot_id || "";
      record.remove.setAttribute("aria-label", `${t("web.remove")} ${role.name}`);
      if (!editingPriority && queueList.children[index] !== record.row) queueList.insertBefore(record.row, queueList.children[index] || null);
      else if (!record.row.parentNode) queueList.append(record.row);
    }
  }
  function isCurrent(plan) {
    const { job, applied } = jobController.state();
    return !applied && job.status === "completed" && plan.valid === true && !job.stale && job.source_snapshot_id === inventory()?.snapshot_id && canRun();
  }
  function allocation(result) {
    const assigned = result.plans.length, unassigned = result.unassigned || [];
    return { assigned, unassigned, total: assigned + unassigned.length,
      incomplete: result.search?.exhaustive !== true,
      budget: ["node_budget", "time_budget", "combination_budget"].includes(result.search?.stop_reason) };
  }
  function increaseBudget() {
    const { active, posting } = jobController.state(); if (active || posting) return;
    for (const field of ["max_nodes", "time_limit_ms", "combination_budget"]) {
      preferences[field] = Math.max(preferences[field], defaults[field]); controls[field].value = field === "time_limit_ms" ? preferences[field] / 1000 : preferences[field];
    }
    advanced.open = true; persist(); controls.time_limit_ms.focus();
  }
  function renderResults() {
    const { job, applied, active, posting } = jobController.state(), result = job.result;
    const key = `${job.id || ""}|${job.status}|${job.stale}|${applied}|${posting}|${inventory()?.snapshot_id}|${Boolean(canRun())}`;
    if (key === renderedOutputKey) return; renderedOutputKey = key;
    output.replaceChildren(); if (applied || job.status !== "completed" || job.kind !== "multi" || !Array.isArray(result?.plans)) return;
    const stale = job.stale || job.source_snapshot_id !== inventory()?.snapshot_id;
    const { assigned, unassigned, total, budget, incomplete } = allocation(result), search = result.search || {};
    const summary = node("div", "search-summary");
    let summaryKey = "web.multi_result_count";
    if (incomplete && unassigned.length) summaryKey = budget ? "web.multi_result_budget_pending" : "web.multi_result_incomplete";
    else if (incomplete) summaryKey = budget ? "web.multi_result_budget_refine" : "web.multi_result_refine";
    summary.append(node("p", `optimizer-result-note${incomplete ? " incomplete" : ""}`, t(summaryKey, assigned, total, unassigned.length)));
    if (unassigned.length) summary.append(node("p", "draft-warning", t(incomplete ? "web.multi_unassigned_pending" : "web.multi_unassigned", unassigned.map(identityName).join(", "))));
    if (incomplete) {
      const increase = node("button", "button secondary", t("web.optimize_increase_budget")); increase.type = "button"; increase.disabled = active || posting; increase.onclick = increaseBudget; summary.append(increase);
    }
    if (stale) summary.append(node("p", "optimizer-result-note incomplete", t("web.optimize_stale_preview")));
    const details = node("details", "search-details"); details.append(node("summary", "", t("web.calculation_details")), node("p", "optimizer-search-stats", t("web.optimize_search_budget", search.nodes ?? "—", search.elapsed_ms == null ? "—" : Math.round(search.elapsed_ms))));
    const stopKey = { node_budget: "web.multi_stop_node_budget", time_budget: "web.multi_stop_time_budget", combination_budget: "web.multi_stop_combination_budget", candidate_limit: "web.multi_stop_candidate_limit", complete: "web.multi_stop_complete" }[search.stop_reason];
    if (stopKey) details.append(node("p", "meta", t(stopKey)));
    if (job.source_snapshot_id) details.append(node("p", "meta mono", t("web.snapshot", job.source_snapshot_id)));
    summary.append(details); output.append(summary);
    const native = result.plans.filter((plan) => plan.native_assemblable !== false && plan.character);
    if (result.plans.length) {
      if (native.length) {
        const resultActions = node("div", "multi-result-actions");
        const applyAll = node("button", "button primary", t("web.apply_plans"));
        applyAll.type = "button";
        applyAll.disabled = !native.every(isCurrent) || native.length > 32;
        applyAll.onclick = () => { if (native.every(isCurrent) && native.length <= 32) applyPlans?.(native); };
        const load = node("button", "button secondary", t(native.length < total ? "web.multi_load_available" : "web.multi_load_all", native.length, total));
        load.type = "button";
        load.disabled = !native.every(isCurrent);
        load.onclick = () => { if (native.every(isCurrent)) loadPlans(native); };
        resultActions.append(applyAll, load);
        output.append(resultActions);
      }
      output.append(createLoadoutSave({ plans: result.plans, sourceSnapshot: job.source_snapshot_id, api, t, node, workspace,
        current: () => result.plans.every(isCurrent), reloadWorkspace, readPreference: readSaveSlots, savePreference: saveSaveSlots }));
    }
    const planGrid = node("div", "multi-plan-grid");
    for (const plan of result.plans) {
      const preview = renderPlanPreview({ plan, t, node, inventory, current: () => isCurrent(plan), loadDraft, saveSoftwarePlan, roleName, title: roleName(plan), showCore: true });
      if (native.length > 1) { const load = preview.querySelector(".button.primary"); load?.classList.replace("primary", "secondary"); }
      planGrid.append(preview);
    }
    output.append(planGrid);
    for (const role of result.role_top_k || []) {
      if (!role.options.length) continue;
      const details = node("details", "role-alternatives");
      details.append(node("summary", "", t("web.multi_alternatives_for", identityName(role.identity))));
      for (const [index, plan] of role.options.entries()) details.append(renderPlanPreview({ plan, t, node, inventory, current: () => isCurrent(plan), loadDraft, saveSoftwarePlan, roleName, title: t("web.optimize_option", index + 1), showCore: true }));
      output.append(details);
    }
  }
  function render() {
    const stored = renderRows();
    const { job, applied, active, posting, cancelling, error: jobError } = jobController.state(), disabled = !loaded || active || posting;
    const confirmed = applied && job.kind === "multi";
    const result = job.status === "completed" && job.kind === "multi" && Array.isArray(job.result?.plans) ? allocation(job.result) : null;
    let phaseText = t(`web.optimize_status_${job.status || "idle"}`);
    if (cancelling) phaseText = t("web.search_progress_cancelling");
    else if (confirmed) phaseText = t("web.operation_confirmed");
    else if (result) phaseText = t(result.budget ? "web.multi_status_budget" : result.incomplete ? "web.multi_status_incomplete" : "web.multi_result_count", result.assigned, result.total);
    phase.textContent = phaseText;
    phase.hidden = active || posting || cancelling || job.kind !== "multi" || job.status === "idle";
    phase.dataset.status = confirmed ? "confirmed" : result?.incomplete ? "incomplete" : job.status || "idle";
    const selected = available().filter((role) => preferences.roles[role.id]?.enabled);
    count.textContent = t("web.multi_selected_count", selected.length);
    renderQueue(selected, stored);
    for (const control of form.querySelectorAll("input,select,button")) control.disabled = disabled || control.dataset.unavailable === "true";
    selectedNames.textContent = selected.map((role) => role.name).join(" · "); selectedNames.title = selectedNames.textContent;
    clearSelection.hidden = active || posting || cancelling || !selected.length; clearSelection.disabled = disabled || !selected.length;
    run.disabled = disabled || !canRun() || !selected.length || Boolean(preferenceError); run.textContent = t(active ? "web.calculating" : "web.multi_run");
    notice.textContent = error || preferenceError || jobError || (job.kind === "multi" && ["failed", "cancelled"].includes(job.status) ? job.message || "" : ""); notice.hidden = !notice.textContent;
    notice.classList.toggle("error", Boolean(error || preferenceError || jobError) || job.status === "failed");
    validateNumbers();
    renderResults();
  }
  form.onsubmit = async (event) => {
    event.preventDefault(); if (!canRun()) return;
    const invalid = form.querySelector(":invalid"); if (invalid) { const details = invalid.closest("details"); if (details) details.open = true; }
    if (!form.reportValidity()) return;
    const roles = available().filter((role) => preferences.roles[role.id]?.enabled);
    if (!roles.length || roles.length > 32) { error = t("web.multi_role_limit"); render(); return; }
    let stored;
    try { stored = rolePreferences(); } catch { error = t("web.optimize_invalid_preferences"); render(); return; }
    const groups = new Map();
    for (const role of roles) {
      const reason = unavailable(role);
      if (reason) { error = t(reason === "web.multi_board_missing" ? "web.multi_board_required" : "web.multi_character_required", role.name); render(); return; }
      const entry = { character: role.character, role_id: role.id, ...(role.official === false ? { allowed_cells: role.allowed_cells } : {}) };
      const resolved = resolveOptimizationPreference(role, stored[role.id]);
      const validWeights = (value) => value && typeof value === "object" && !Array.isArray(value) && Object.values(value).every((weight) => Number.isFinite(weight) && weight >= 0 && weight <= 1);
      if (resolved.require_core !== false && resolved.core_suit_source === "recommended" && !resolved.core_suit_id) { error = t("web.optimize_recommended_suit_missing", role.name); render(); return; }
      if (!validWeights(resolved.weights) || (resolved.shared_main_weights !== true && resolved.main_weights != null && !validWeights(resolved.main_weights))) { error = t("web.multi_invalid_weights", role.name); render(); return; }
      if (!Object.values(resolved.weights).some((value) => value > 0)) { error = t("web.multi_weights_required", role.name); render(); return; }
      Object.assign(entry, optimizationPreferenceFields(resolved, inventory()?.items || [], role.character));
      const preference = preferences.roles[role.id], slot = (workspace()?.slots || []).find((slot) => slot.role_id === role.id && slot.id === preference.keep_slot_id);
      if (preference.keep_slot_id) {
        if (!slot?.plan || slot.archived) { error = t("web.multi_missing_keep", role.name); render(); return; }
        entry.keep = { ...(entry.require_core ? { core: slot.plan.core } : {}), placements: slot.plan.placements };
      }
      if (!groups.has(preference.priority)) groups.set(preference.priority, []); groups.get(preference.priority).push(entry);
    }
    error = "";
    await jobController.run("api/optimize", { snapshot_id: inventory().snapshot_id, groups: [...groups].sort(([left], [right]) => right - left).map(([, roles]) => roles),
      top_k: preferences.top_k, include_role_top_k: preferences.include_role_top_k, combination_budget: preferences.combination_budget,
      max_nodes: preferences.max_nodes, time_limit_ms: preferences.time_limit_ms, worker_count: preferences.worker_count,
      ...(preferences.exclude_selected ? { exclude_uids: selectedUids() } : {}) });
  };
  jobController.subscribe(render);
  return { activate() { if (!loaded || accountId !== workspace()?.account?.id) initialize(); render(); }, sync() { if (!workspace()?.account) return; if (!loaded || accountId !== workspace().account.id) initialize(); render(); } };
}
