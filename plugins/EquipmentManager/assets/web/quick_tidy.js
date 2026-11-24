import { generalWeights, recommendedWeightsForRole, warehouseScoringRoles } from "./weight_preferences.js";

const grades = ["D", "C", "B", "A", "S", "SS", "SSS", "ACE"];
export function createQuickTidy({ api, t, node, workspace, inventory, currentRole, jobController, canRun,
  reloadWorkspace, refreshInventory, trackOperation, cancelOperations, readPreference, savePreference, readRolePreferences, saveRolePreferences, openAdvanced }) {
  let preferences = { scope: "all", role_id: "", lock_enabled: true, lock_grade: "A", discard_enabled: false, discard_grade: "C" };
  let timer, error = "", applying = false, applied = "", ownRules = "", ownPreviewId = "", operationId = null, appliedCounts = null;
  try { const saved = JSON.parse(readPreference() || "{}"); preferences = { ...preferences, ...saved }; }
  catch { error = t("web.rules_invalid_preferences"); }
  if (!["all", "role"].includes(preferences.scope)) preferences.scope = "all";
  const dialog = node("dialog", "tidy-dialog"), heading = node("div", "tidy-heading");
  dialog.setAttribute("aria-label", t("web.tidy_title"));
  const close = node("button", "icon-button", "×"); close.type = "button"; close.setAttribute("aria-label", t("web.dismiss")); close.onclick = () => dialog.close();
  heading.append(node("h2", "", t("web.tidy_title")), close);
  const content = node("div", "tidy-content"), roleLabel = node("label", "page-field"), roleSelect = node("select");
  roleLabel.append(node("span", "", t("web.tidy_score_for")), roleSelect); roleSelect.onchange = () => {
    preferences.scope = roleSelect.value === "all" ? "all" : "role";
    preferences.role_id = roleSelect.value.startsWith("role:") ? roleSelect.value.slice(5) : ""; changed();
  };
  const fields = {};
  for (const [kind, caption] of [["lock", "web.tidy_lock_at"], ["discard", "web.tidy_discard_at"]]) {
    const row = node("label", "tidy-threshold"), enabled = node("input"), grade = node("select"); enabled.type = "checkbox"; enabled.checked = preferences[`${kind}_enabled`] === true;
    for (const value of grades) { const option = node("option", "", value); option.value = value; grade.append(option); }
    grade.value = grades.includes(preferences[`${kind}_grade`]) ? preferences[`${kind}_grade`] : kind === "lock" ? "A" : "C";
    enabled.onchange = () => { preferences[`${kind}_enabled`] = enabled.checked; changed(); };
    grade.onchange = () => { preferences[`${kind}_grade`] = grade.value; changed(); };
    row.append(enabled, node("span", "", t(caption)), grade); fields[kind] = { enabled, grade }; content.append(row);
  }
  const weights = node("div", "tidy-weights"), useGeneral = node("button", "button secondary", t("web.use_general_preference")); useGeneral.type = "button";
  weights.append(node("p", "meta", t("web.tidy_needs_preference")), useGeneral);
  const coverage = node("p", "meta"), protection = node("p", "tidy-protection", t("web.tidy_protection")), counts = node("div", "tidy-counts"), notice = node("p", "tidy-message"); notice.setAttribute("role", "status");
  const actions = node("div", "tidy-actions"), preview = node("button", "button secondary", t("web.refresh_preview")), apply = node("button", "button primary", t("web.tidy_apply")), cancel = node("button", "button quiet", t("web.cancel_pending"));
  preview.type = apply.type = cancel.type = "button"; preview.onclick = requestPreview; cancel.onclick = () => operationId == null ? jobController.cancel() : cancelOperations();
  const advanced = node("button", "button quiet", t("web.advanced_rules")); advanced.type = "button"; advanced.onclick = () => { dialog.close(); openAdvanced(); };
  content.prepend(roleLabel, coverage); content.append(weights, protection, counts, notice); actions.append(preview, apply, cancel); dialog.append(heading, content, actions, advanced); document.body.append(dialog);
  function roles() {
    let stored;
    try { stored = JSON.parse(readRolePreferences() || "{}"); } catch { stored = null; }
    return warehouseScoringRoles(workspace(), stored);
  }
  const selectedRole = () => roles().find((role) => role.id === (preferences.role_id || currentRole()));
  const targetRoles = () => preferences.scope === "all" ? roles() : [selectedRole()].filter(Boolean);
  const hasWeights = () => targetRoles().some((role) => role.scoring_weights_complete);
  const canonical = (rules) => JSON.stringify((rules || []).map((rule) => [rule.id, rule.action, rule.min_grade || "", rule.max_grade || "", rule.role_ids || []]));
  function rules() {
    const result = [], selected = targetRoles();
    if (!selected.length) return result;
    const role_ids = selected.map((role) => role.id);
    if (preferences.lock_enabled) result.push({ id: "quick-lock", enabled: true, action: "lock", min_grade: fields.lock.grade.value, role_ids, on_locked: "skip" });
    if (preferences.discard_enabled) result.push({ id: "quick-discard", enabled: true, action: "discard", max_grade: fields.discard.grade.value, role_ids, on_locked: "skip" });
    return result;
  }
  function matchingPreview() {
    const { job } = jobController.state();
    return job.kind === "warehouse_rules" && job.status === "completed" && ownPreviewId && job.id === ownPreviewId && job.result?.preview_id === ownPreviewId && ownRules && canonical(job.result?.rules_snapshot) === ownRules && ownRules === canonical(rules()) ? job.result : null;
  }
  function changed() {
    clearTimeout(timer); error = ""; operationId = null; ownPreviewId = ""; savePreference(JSON.stringify(preferences)); render(); timer = setTimeout(requestPreview, 350);
  }
  async function requestPreview() {
    clearTimeout(timer);
    const { active, posting } = jobController.state();
    if (!dialog.open || applying || active || posting || !canRun() || !hasWeights() || (!preferences.lock_enabled && !preferences.discard_enabled)) { render(); return; }
    error = ""; const ordered = rules(); ownRules = canonical(ordered); ownPreviewId = "";
    const accepted = await jobController.run("api/warehouse/rules/preview", { snapshot_id: inventory().snapshot_id, rules: ordered, include_equipped: false });
    ownPreviewId = accepted?.id || "";
    render();
  }
  useGeneral.onclick = () => {
    const role = selectedRole(); if (!role || applying || preferences.scope === "all") return;
    const preset = recommendedWeightsForRole(role) || generalWeights();
    try {
      const stored = JSON.parse(readRolePreferences() || "{}");
      if (!stored || typeof stored !== "object" || Array.isArray(stored)) throw new Error(t("web.optimize_invalid_preferences"));
      stored[role.id] = { ...(stored[role.id] || {}), weights: { ...preset.weights }, main_weights: { ...preset.main_weights },
        shared_main_weights: false, preset: recommendedWeightsForRole(role) ? "recommended" : "general" };
      saveRolePreferences(JSON.stringify(stored));
      clearTimeout(timer); error = ""; ownPreviewId = ""; render(); timer = setTimeout(requestPreview, 800);
    } catch (failure) { error = failure.message; render(); }
  };
  apply.onclick = async () => {
    const result = matchingPreview(), { job } = jobController.state();
    if (!result || !canRun() || job.stale || result.source_snapshot_id !== inventory()?.snapshot_id || applying || applied === result.preview_id) return;
    applying = true; error = ""; render();
    try { const accepted = await api("api/warehouse/rules/apply", { preview_id: result.preview_id, snapshot_id: result.source_snapshot_id }); applied = result.preview_id; operationId = accepted.id; appliedCounts = { lock: result.changes.filter((change) => change.state === "lock").length, discard: result.changes.filter((change) => change.state === "discard").length }; trackOperation(accepted.id); await refreshInventory(); }
    catch (failure) { error = failure.message; }
    finally { applying = false; render(); }
  };
  function render() {
    if (!dialog.open) return;
    const available = roles(), selected = targetRoles(), missing = selected.filter((role) => !role.scoring_weights_complete);
    const values = [["all", t("web.tidy_all_roles")], ["current", t("web.tidy_current_role")], ...available.map((role) => [`role:${role.id}`, role.name])];
    if (JSON.stringify([...roleSelect.options].map((option) => [option.value, option.textContent])) !== JSON.stringify(values)) roleSelect.replaceChildren(...values.map(([id, label]) => { const option = node("option", "", label); option.value = id; return option; }));
    roleSelect.value = preferences.scope === "all" ? "all" : preferences.role_id ? `role:${preferences.role_id}` : "current";
    coverage.textContent = selected.length ? `${t("web.tidy_scoring_coverage", selected.length)}${preferences.scope === "all" ? ` · ${t("web.tidy_all_roles_hint")}` : ""}` : t("web.tidy_no_roles");
    const { job, active, posting, error: jobError } = jobController.state(), result = matchingPreview();
    const operation = (inventory()?.operations || []).find((value) => String(value.id) === String(operationId));
    const pendingOperation = operationId != null && (!operation || ["queued", "running", "dispatched"].includes(operation.status));
    const disabled = applying || active || posting || pendingOperation;
    roleSelect.disabled = disabled; for (const field of Object.values(fields)) { field.enabled.disabled = disabled; field.grade.disabled = disabled || !field.enabled.checked; }
    weights.hidden = !missing.length;
    weights.firstChild.textContent = preferences.scope === "all" ? t("web.tidy_missing_weights", missing.length, missing.map((role) => role.name || role.id).join(", ")) : t("web.tidy_needs_preference");
    useGeneral.textContent = t(recommendedWeightsForRole(selectedRole()) ? "web.use_recommended_preference" : "web.use_general_preference");
    useGeneral.hidden = preferences.scope === "all" || !selectedRole(); useGeneral.disabled = disabled;
    counts.replaceChildren();
    if (result) for (const [key, value] of [["web.tidy_will_lock", result.changes.filter((change) => change.state === "lock").length], ["web.tidy_will_discard", result.changes.filter((change) => change.state === "discard").length], ["web.tidy_skip", Object.values(result.skipped || {}).reduce((sum, value) => sum + value, 0)]]) { const metric = node("div"); metric.append(node("strong", "", value), node("span", "", t(key))); counts.append(metric); }
    const operationStatus = { confirmed: "web.operation_confirmed", failed: "web.operation_failed", unknown: "web.operation_unknown", cancelled: "web.operation_cancelled" }[operation?.status] || "web.operation_working";
    notice.textContent = error || (operationId != null ? operation ? `${t(operationStatus)}${operation.status === "confirmed" && appliedCounts ? ` · ${t("web.tidy_applied_counts", appliedCounts.lock, appliedCounts.discard)}` : ["failed", "unknown"].includes(operation.status) && operation.message ? ` · ${operation.message}` : ""}` : t("web.operation_pending") : jobError || (active ? t(job.id === ownPreviewId || posting && ownRules ? "web.tidy_preparing" : "web.other_calculation_busy") : !selected.length ? t("web.tidy_no_roles") : result && job.stale ? t("web.rules_stale") : result && !result.complete ? t("web.tidy_skipped_unknown") : ownPreviewId && job.id !== ownPreviewId ? t("web.tidy_preview_replaced") : ""));
    notice.classList.toggle("error", Boolean(error || jobError) || ["failed", "unknown"].includes(operation?.status)); notice.classList.toggle("success", operation?.status === "confirmed"); notice.hidden = !notice.textContent;
    preview.disabled = disabled || !canRun() || !hasWeights();
    apply.disabled = disabled || !canRun() || !result || job.stale || result.source_snapshot_id !== inventory()?.snapshot_id || !result.changes.length || applied === result.preview_id;
    cancel.hidden = !(active && job.id === ownPreviewId) && !pendingOperation; cancel.disabled = applying || posting;
  }
  dialog.addEventListener("close", () => clearTimeout(timer)); jobController.subscribe(render);
  return { open() {
    if (!dialog.open) dialog.showModal();
    const { job, active } = jobController.state();
    if (!active || job.id !== ownPreviewId) { ownRules = ""; ownPreviewId = ""; }
    const operation = (inventory()?.operations || []).find((value) => String(value.id) === String(operationId));
    if (canRun() || operation && !["queued", "dispatched", "running"].includes(operation.status)) operationId = null;
    render(); timer = setTimeout(requestPreview, 150);
  }, sync: render };
}
