import { createProfileForm } from "./profile_form.js";
import { createRoleComparisons } from "./role_comparisons.js";
import { propertyLabel } from "./equipment_display.js";

const elements = ["DamageUpChaosBase", "DamageUpCosmosBase", "DamageUpIncantationBase", "DamageUpLakshanaBase", "DamageUpNatureBase", "DamageUpPsycheBase", "DamageUpPsychicallyBase"];
export function createRolePanel({ mount, api, t, node, workspace, inventory, features, jobController, reloadWorkspace, readPreference, savePreference, inspectItem }) {
  let preferences = { role_id: "", slot_id: "", damage_property: "", enemy_mode: "neutral", defense_base: "", resistance: "", crit_cap: "" }, loaded = false, saving = false, dirty = false, error = "", signature = "", profileForm, renderedOutputKey = "";
  const heading = node("div", "page-heading"); heading.append(node("div")); heading.firstChild.append(node("h2", "", t("web.panel_title")), node("p", "meta", t("web.panel_hint")));
  const phase = node("span", "badge"); heading.append(phase);
  const toolbar = node("div", "page-toolbar"), role = node("select"), slot = node("select"); role.setAttribute("aria-label", t("web.character")); slot.setAttribute("aria-label", t("web.panel_slot")); toolbar.append(role, slot);
  const profileSection = node("details", "profile-section"), form = node("form", "profile-form"), profileContent = node("div"), save = node("button", "button primary", t("web.profile_save")), discard = node("button", "button quiet", t("web.roles_discard")); save.type = "submit"; discard.type = "button";
  const profileActions = node("div", "page-actions"); profileActions.append(save, discard); form.append(profileContent, profileActions); profileSection.append(node("summary", "", t("web.profile_title")), form);
  const calculation = node("form", "panel-calculation"), options = node("div", "tool-options"), fields = {};
  for (const [field, key, entries] of [["damage_property", "web.panel_damage_property", [["", "web.panel_no_damage"], ...elements.map((id) => [id, null])]], ["enemy_mode", "web.panel_enemy_model", [["neutral", "web.panel_enemy_neutral"], ["explicit", "web.panel_enemy_explicit"]]]]) {
    const label = node("label", "page-field"), select = node("select");
    for (const [value, caption] of entries) { const option = node("option", "", caption ? t(caption) : propertyLabel(t, value)); option.value = value; select.append(option); }
    select.onchange = () => { preferences[field] = select.value; persist(); }; fields[field] = select; label.append(node("span", "", t(key)), select); options.append(label);
  }
  for (const [field, key] of [["defense_base", "web.panel_enemy_defense"], ["resistance", "web.panel_enemy_resistance"], ["crit_cap", "web.panel_crit_cap"]]) {
    const label = node("label", "page-field"), input = node("input"); input.type = "number"; input.step = "any"; if (field !== "resistance") input.min = "0"; if (field === "crit_cap") input.max = "100";
    input.oninput = () => { if (input.checkValidity()) { preferences[field] = input.value; persist(); } }; fields[field] = input; label.append(node("span", "", t(key)), input); options.append(label);
  }
  const actions = node("div", "page-actions"), run = node("button", "button primary", t("web.panel_run")), cancel = node("button", "button quiet", t("web.optimize_cancel")); run.type = "submit"; cancel.type = "button"; cancel.onclick = () => jobController.cancel(); actions.append(run, cancel);
  calculation.append(options, node("p", "meta", t("web.panel_neutral_hint")), actions);
  const notice = node("p", "tool-message"); notice.setAttribute("role", "status"); const output = node("div", "panel-results"); mount.append(heading, toolbar, profileSection, calculation, notice, output);
  const selectedRole = () => workspace()?.roles?.find((value) => value.id === preferences.role_id && value.official !== false && !value.archived);
  const comparisons = createRoleComparisons({ controlsMount: calculation, resultMount: mount, t, node, inventory, workspace, selectedRole,
    selectedSlot: () => preferences.slot_id, preferences: () => preferences, persist, features, jobController, inspectItem });
  function persist() { error = ""; savePreference(JSON.stringify(preferences)); render(); }
  function initialize() {
    try { const saved = JSON.parse(readPreference() || "{}"); if (!saved || typeof saved !== "object" || Array.isArray(saved)) throw new Error(); preferences = { ...preferences, ...saved }; }
    catch { error = t("web.panel_invalid_preferences"); }
    for (const [field, input] of Object.entries(fields)) input.value = preferences[field]; loaded = true;
  }
  function catalog() {
    const roles = (workspace()?.roles || []).filter((value) => !value.archived && value.official !== false), slots = workspace()?.slots || [];
    const statNames = new Map();
    for (const item of inventory()?.items || []) for (const stat of item.attributes || []) if (stat.id) statNames.set(stat.id, propertyLabel(t, stat));
    for (const option of fields.damage_property.options) if (option.value) option.textContent = statNames.get(option.value) || propertyLabel(t, option.value);
    if (dirty || saving) return;
    if (!roles.some((value) => value.id === preferences.role_id)) preferences.role_id = roles[0]?.id || "";
    const next = JSON.stringify([roles, slots.map((slot) => [slot.id, slot.role_id, slot.name, slot.current_revision])]);
    if (signature === next) return; signature = next;
    role.replaceChildren(...roles.map((value) => { const option = node("option", "", value.name); option.value = value.id; return option; })); role.value = preferences.role_id;
    slot.replaceChildren(...[["", t("web.panel_current_equipment")], ...slots.filter((slot) => slot.role_id === preferences.role_id && !slot.archived && slot.plan).map((slot) => [slot.id, slot.name || t("web.library_primary")])].map(([value, label]) => { const option = node("option", "", label); option.value = value; return option; })); slot.value = preferences.slot_id;
    if (!slot.value) preferences.slot_id = "";
    const value = selectedRole(); profileContent.replaceChildren();
    if (value) { profileForm = createProfileForm({ node, t, profile: value.profile || {}, changed: () => { dirty = true; render(); } }); profileContent.append(profileForm.element); }
  }
  role.onchange = () => { if (dirty) { role.value = preferences.role_id; error = t("web.roles_unsaved"); render(); return; } preferences.role_id = role.value; preferences.slot_id = ""; signature = ""; catalog(); persist(); };
  slot.onchange = () => { preferences.slot_id = slot.value; persist(); };
  discard.onclick = () => { dirty = false; signature = ""; error = ""; catalog(); render(); };
  form.onsubmit = async (event) => {
    event.preventDefault(); if (!profileForm || saving || !form.reportValidity()) return;
    saving = true; error = ""; render();
    try { await api("api/roles", { action: "update", id: preferences.role_id, role: { profile: profileForm.value() } }); await reloadWorkspace(); dirty = false; signature = ""; }
    catch (failure) { error = failure.message; }
    finally { saving = false; catalog(); render(); }
  };
  calculation.onsubmit = async (event) => {
    event.preventDefault(); if (dirty) { error = t("web.panel_save_profile_first"); render(); return; } if (!selectedRole() || !calculation.reportValidity()) return;
    const request = { role_id: preferences.role_id, ...(preferences.slot_id ? { slot_id: preferences.slot_id } : {}) };
    if (preferences.damage_property) {
      const enemy = preferences.enemy_mode === "neutral" ? { neutral: true } : { defense_base: Number(preferences.defense_base), resistance: Number(preferences.resistance) / 100 };
      request.damage = { property: preferences.damage_property, enemy, ...(preferences.crit_cap === "" ? {} : { crit_cap: Number(preferences.crit_cap) / 100 }) };
    }
    Object.assign(request, comparisons.extraRequest());
    error = ""; await jobController.run(`api/roles/${comparisons.kind()}`, request);
  };
  function renderResults() {
    const { job } = jobController.state(), result = job.result;
    const stale = job.stale || job.profile_stale;
    const key = `${job.id || ""}|${job.status}|${job.stale}|${job.profile_stale}|${saving}|${dirty}`; if (key === renderedOutputKey) return; renderedOutputKey = key;
    output.replaceChildren(); if (job.status !== "completed" || job.kind !== "panel" || !result) return;
    if (result.role_id) output.append(node("p", "meta", t("web.panel_result_for", workspace()?.roles?.find((role) => role.id === result.role_id)?.name || result.role_id)));
    output.append(node("p", result.complete === true ? "meta" : "draft-warning", t(result.complete === true ? "web.panel_complete" : "web.panel_incomplete")));
    if (job.stale && !job.profile_stale) output.append(node("p", "draft-warning", t("web.optimize_stale_preview")));
    if (job.profile_stale) output.append(node("p", "draft-warning", t("web.panel_profile_stale")));
    if (result.equipment_projection === "maximum_level") output.append(node("p", "meta", t("web.panel_max_projection")));
    if (result.missing?.length) output.append(node("p", "meta", t("web.panel_missing", result.missing.map((value) => typeof value === "string" ? value : value.source).join(", "))));
    const format = (value, percent = false) => value == null ? "—" : new Intl.NumberFormat(document.documentElement.lang || "en", percent ? { style: "percent", maximumFractionDigits: 2 } : { maximumFractionDigits: 2 }).format(value);
    const metadata = new Map((result.attributes_metadata || []).map((stat) => [stat.id, stat]));
    const stats = Array.isArray(result.stats) ? result.stats : Object.entries(result.attributes || {}).map(([id, value]) => ({ ...metadata.get(id), id, value }));
    const table = node("table", "data-table"), body = node("tbody");
    const formatStat = (value, id, explicit) => {
      if (value == null) return "—";
      const meta = typeof explicit?.percent === "boolean" ? explicit : metadata.get(id);
      return meta?.percent === true ? format(value, true) : meta?.percent === false ? format(value) : t("web.panel_raw_value", format(value));
    };
    for (const stat of stats) { const row = node("tr"); row.append(node("th", "", propertyLabel(t, stat)), node("td", "", formatStat(stat.value, stat.id, stat))); body.append(row); } table.append(body); output.append(table);
    if (result.derived) { const details = node("details", "panel-source"); details.append(node("summary", "", t("web.panel_derived")), node("pre", "mono", JSON.stringify(result.derived, null, 2))); output.append(details); }
    if (result.direct_damage) output.append(node("p", "panel-damage", t("web.panel_direct_damage", format(result.direct_damage.value))));
    if (Array.isArray(result.margins)) {
      const table = node("table", "data-table"), head = node("thead"), row = node("tr"); for (const key of ["web.optimize_property", "web.panel_current_value", "web.panel_unit", "web.panel_gain"]) row.append(node("th", "", t(key))); head.append(row); table.append(head);
      const body = node("tbody"); for (const margin of result.margins) { const row = node("tr"); row.append(node("th", "", propertyLabel(t, metadata.get(margin.id) || margin.id)), node("td", "", formatStat(margin.current_value, margin.id)), node("td", "", formatStat(margin.unit, margin.id)), node("td", "", t("web.panel_percent_value", format(margin.gain_percent)))); body.append(row); } table.append(body); output.append(table);
    }
    if (result.dynamic_weights && result.marginal_available !== false && result.role_id) {
      const saveWeights = node("button", "button quiet", t("web.panel_save_weights")); saveWeights.type = "button"; saveWeights.disabled = saving || dirty || stale;
      saveWeights.onclick = async () => {
        if (saving || dirty || stale) return; saving = true; render();
        try { await api("api/roles", { action: "update", id: result.role_id, role: { weights: result.dynamic_weights, ...(result.dynamic_main_weights ? { main_weights: result.dynamic_main_weights } : {}) } }); await reloadWorkspace(); }
        catch (failure) { error = failure.message; }
        finally { saving = false; render(); }
      }; output.append(saveWeights);
    }
    if (result.graduation) output.append(node("p", "meta", t("web.panel_graduation", result.graduation.available ? format(result.graduation.rate, true) : "—")));
    if (result.sources) { const details = node("details", "panel-source"); details.append(node("summary", "", t("web.panel_sources")), node("pre", "mono", JSON.stringify(result.sources, null, 2))); output.append(details); }
    if (result.profile) { const details = node("details", "panel-source"); details.append(node("summary", "", t("web.panel_profile_source")), node("pre", "mono", JSON.stringify(result.profile, null, 2))); output.append(details); }
  }
  function render() {
    const { job, active, posting, error: jobError } = jobController.state(), disabled = saving || active || posting || !loaded;
    role.disabled = disabled; slot.disabled = disabled; save.disabled = disabled || !selectedRole(); discard.disabled = disabled || !dirty;
    for (const control of profileContent.querySelectorAll("input,select,textarea,button")) control.disabled = disabled;
    profileForm?.setDisabled(disabled);
    for (const control of options.querySelectorAll("input,select")) control.disabled = disabled;
    const explicit = preferences.damage_property && preferences.enemy_mode === "explicit";
    for (const field of ["defense_base", "resistance"]) { fields[field].disabled = disabled || !explicit; fields[field].required = Boolean(explicit); }
    fields.crit_cap.disabled = disabled || !preferences.damage_property;
    fields.damage_property.required = comparisons.kind() !== "panel";
    comparisons.render(disabled);
    run.textContent = t(comparisons.kind() === "contributions" ? "web.comparison_run_contributions" : comparisons.kind() === "replacements" ? "web.comparison_run_replacements" : "web.panel_run");
    run.disabled = disabled || dirty || !selectedRole(); cancel.disabled = !active || posting;
    phase.textContent = t(`web.optimize_status_${job.status || "idle"}`); notice.textContent = error || jobError || (["panel", "contributions", "replacements"].includes(job.kind) && job.status === "failed" ? job.message : "") || ""; notice.hidden = !notice.textContent; notice.classList.toggle("error", Boolean(error || jobError) || job.status === "failed"); renderResults();
  }
  jobController.subscribe(render);
  return {
    selectRole(id) { if (!loaded) initialize(); if (dirty) { error = t("web.roles_unsaved"); render(); return; } preferences.role_id = id; preferences.slot_id = ""; signature = ""; persist(); catalog(); render(); },
    activate() { if (!loaded) initialize(); catalog(); render(); },
    sync() { if (!workspace()?.account) return; if (!loaded) initialize(); catalog(); render(); },
  };
}
