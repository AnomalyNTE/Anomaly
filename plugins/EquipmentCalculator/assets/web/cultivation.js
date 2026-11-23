import { createProfileForm } from "./profile_form.js";

export function createCultivationPanel({ mount, t, node, workspace, jobController, isReady, readPreference, savePreference }) {
  let preferences = { plans: [], allow_crafting: true }, loaded = false, error = "", renderedOutputKey = "", roleSignature = "";
  const editors = [];
  const heading = node("div", "page-heading"); heading.append(node("div")); heading.firstChild.append(node("h2", "", t("web.cultivation_title")), node("p", "meta", t("web.cultivation_hint")));
  const phase = node("span", "badge"); heading.append(phase);
  const form = node("form", "cultivation-form"), toolbar = node("div", "cultivation-add"), select = node("select"), add = node("button", "button secondary", t("web.cultivation_add"));
  select.setAttribute("aria-label", t("web.character")); add.type = "button";
  const cards = node("div", "cultivation-plans"), craftLabel = node("label", "optimizer-check"), crafting = node("input"); crafting.type = "checkbox";
  craftLabel.append(crafting, node("span", "", t("web.cultivation_crafting"))); crafting.onchange = () => { preferences.allow_crafting = crafting.checked; persist(); };
  toolbar.append(select, add); form.append(toolbar, cards, craftLabel);
  const actions = node("div", "page-actions"), run = node("button", "button primary", t("web.cultivation_run")), cancel = node("button", "button quiet", t("web.optimize_cancel")); run.type = "submit"; cancel.type = "button"; cancel.onclick = () => jobController.cancel(); actions.append(run, cancel); form.append(actions);
  const notice = node("p", "tool-message"); notice.setAttribute("role", "status"); const output = node("div", "cultivation-results"); mount.append(heading, form, notice, output);
  const roles = () => (workspace()?.roles || []).filter((role) => !role.archived && role.official !== false);
  function persist() { error = ""; savePreference(JSON.stringify(preferences)); render(); }
  function initialize() {
    try {
      const saved = JSON.parse(readPreference() || "{}");
      if (!saved || typeof saved !== "object" || Array.isArray(saved) || (Object.hasOwn(saved, "plans") && (!Array.isArray(saved.plans) || saved.plans.length > 32 || !saved.plans.every((plan) => plan && typeof plan.id === "string" && typeof plan.role_id === "string")))) throw new Error();
      preferences = { ...preferences, ...saved };
    } catch { error = t("web.cultivation_invalid_preferences"); }
    crafting.checked = preferences.allow_crafting === true; loaded = true; renderCards();
  }
  function saveEditors() {
    if (!form.checkValidity()) { error = t("web.cultivation_invalid_fields"); render(); return; }
    try {
      for (const { plan, from, to } of editors) { if (!plan.use_observed) plan.from = from.value(); plan.to = to.value(); }
      persist();
    } catch (failure) { error = failure.message; render(); }
  }
  function renderCards() {
    editors.length = 0; cards.replaceChildren();
    for (const [index, plan] of preferences.plans.entries()) {
      const role = roles().find((role) => role.id === plan.role_id), card = node("section", "cultivation-plan");
      const header = node("div", "cultivation-plan-heading"); header.append(node("h3", "", role?.name || plan.role_id));
      const move = (offset) => { if (!form.checkValidity()) { error = t("web.cultivation_invalid_fields"); render(); return; } const other = index + offset; [preferences.plans[index], preferences.plans[other]] = [preferences.plans[other], preferences.plans[index]]; renderCards(); persist(); };
      for (const [label, action, disabled] of [["web.cultivation_earlier", () => move(-1), index === 0], ["web.cultivation_later", () => move(1), index === preferences.plans.length - 1], ["web.remove", () => { preferences.plans.splice(index, 1); renderCards(); persist(); }, false]]) {
        const button = node("button", "button quiet", t(label)); button.type = "button"; button.disabled = disabled; button.dataset.orderDisabled = String(disabled); button.onclick = action; header.append(button);
      }
      const observedLabel = node("label", "optimizer-check"), observed = node("input"); observed.type = "checkbox"; observed.checked = plan.use_observed !== false;
      observedLabel.append(observed, node("span", "", t("web.cultivation_use_observed"))); observed.onchange = () => { plan.use_observed = observed.checked; fromSection.open = !observed.checked; persist(); };
      const columns = node("div", "cultivation-profile-columns"), fromSection = node("details", "cultivation-profile"), toSection = node("details", "cultivation-profile");
      fromSection.append(node("summary", "", t("web.cultivation_from"))); fromSection.open = plan.use_observed === false; toSection.append(node("summary", "", t("web.cultivation_to"))); toSection.open = true;
      const from = createProfileForm({ node, t, profile: plan.from || {}, mode: "cultivation", changed: saveEditors }), to = createProfileForm({ node, t, profile: plan.to || {}, mode: "cultivation", changed: saveEditors });
      fromSection.append(from.element); toSection.append(to.element); columns.append(fromSection, toSection);
      const observedDetails = node("details", "cultivation-observed"); observedDetails.append(node("summary", "", t("web.cultivation_observed_source")), node("pre", "mono", JSON.stringify(role?.observed || null, null, 2)));
      card.append(header, observedLabel, observedDetails, columns); cards.append(card); editors.push({ plan, from, to, fromSection, observedDetails });
    }
    if (!preferences.plans.length) cards.append(node("p", "empty", t("web.cultivation_empty")));
  }
  add.onclick = () => {
    if (!form.checkValidity()) { error = t("web.cultivation_invalid_fields"); render(); return; }
    const role = roles().find((role) => role.id === select.value); if (!role || preferences.plans.length >= 32 || preferences.plans.some((plan) => plan.role_id === role.id)) return;
    preferences.plans.push({ id: `growth-${role.id}`, role_id: role.id, use_observed: true, from: structuredClone(role.profile || {}), to: {} }); roleSignature = ""; renderCards(); catalog(); persist();
  };
  function catalog() {
    const available = roles().filter((role) => !preferences.plans.some((plan) => plan.role_id === role.id)), signature = JSON.stringify(available.map((role) => [role.id, role.name]));
    if (roleSignature === signature) return; roleSignature = signature;
    const selected = select.value; select.replaceChildren(...available.map((role) => { const option = node("option", "", role.name); option.value = role.id; return option; }));
    if ([...select.options].some((option) => option.value === selected)) select.value = selected;
  }
  function quantities(required, owned, deficits, allocated = false) {
    const table = node("table", "data-table"), head = node("thead"), header = node("tr");
    for (const key of ["web.cultivation_material", "web.cultivation_required", ...(allocated ? ["web.cultivation_allocated", "web.cultivation_deficit"] : [])]) header.append(node("th", "", t(key))); head.append(header); table.append(head);
    const body = node("tbody");
    for (const [id, value] of Object.entries(required || {})) { const row = node("tr"); row.append(node("th", "mono", id), node("td", "", value ?? "—")); if (allocated) row.append(node("td", "", owned?.[id] ?? "—"), node("td", "", deficits?.[id] ?? "—")); body.append(row); }
    table.append(body); return table;
  }
  function renderResults() {
    const { job } = jobController.state(), result = job.result;
    const key = `${job.id || ""}|${job.status}|${job.stale}`; if (key === renderedOutputKey) return; renderedOutputKey = key;
    output.replaceChildren(); if (job.status !== "completed" || job.kind !== "cultivation" || !result) return;
    output.append(node("p", result.status === "complete" ? "meta" : "draft-warning", t(result.status === "complete" ? "web.cultivation_complete" : result.status === "unavailable" ? "web.cultivation_unavailable" : "web.cultivation_partial")), node("p", "meta", t("web.cultivation_frozen")));
    if (job.stale) output.append(node("p", "draft-warning", t("web.cultivation_stale")));
    if (result.owned_complete === false || result.gaps?.includes("owned_materials_unavailable")) output.append(node("p", "draft-warning", t("web.cultivation_owned_unavailable")));
    if (result.materials) output.append(node("h3", "", t("web.cultivation_total")), quantities(result.materials));
    else if (result.known_materials) output.append(node("h3", "", t("web.cultivation_known_subtotal")), node("p", "draft-warning", t("web.cultivation_subtotal_hint")), quantities(result.known_materials));
    if (result.gold == null && result.known_gold != null) output.append(node("p", "meta", t("web.cultivation_known_gold", result.known_gold)));
    for (const plan of result.plans || []) {
      const details = node("details", "cultivation-plan-result"), roleId = plan.role_id || String(plan.id).replace(/^growth-/, ""), role = workspace()?.roles?.find((role) => role.id === roleId);
      details.append(node("summary", "", role?.name || plan.id));
      if (plan.materials == null && plan.known_materials) details.append(node("p", "draft-warning", t("web.cultivation_subtotal_hint")));
      if (plan.owned_allocation_complete === false && result.owned_complete !== false) details.append(node("p", "draft-warning", t("web.cultivation_allocation_partial")));
      details.append(quantities(plan.materials ?? plan.known_materials, plan.owned_allocated, plan.deficits ?? plan.known_deficits, true));
      if (plan.gold == null && plan.known_gold != null) details.append(node("p", "meta", t("web.cultivation_known_gold", plan.known_gold)));
      for (const [field, key] of [["tracks", "web.cultivation_transitions"], ["crafts", "web.cultivation_crafts"]]) if (plan[field]) { const data = node("details", "catalog-nested"); data.append(node("summary", "", t(key)), node("pre", "mono", JSON.stringify(plan[field], null, 2))); details.append(data); }
      output.append(details);
    }
    if (result.source_tracks) {
      const source = node("details", "panel-source"); source.append(node("summary", "", t("web.cultivation_source_tracks")));
      const table = node("table", "data-table"), head = node("thead"), header = node("tr"); for (const key of ["web.character", "web.cultivation_track", "web.cultivation_current", "web.cultivation_target", "web.cultivation_experience_scope"]) header.append(node("th", "", t(key))); head.append(header); table.append(head); const body = node("tbody");
      for (const plan of result.source_tracks) for (const track of plan.tracks || []) {
        const role = workspace()?.roles?.find((role) => role.id === plan.role_id), row = node("tr");
        const scope = track.experience_scope === "full_level_interval" ? t("web.cultivation_scope_full") : track.experience_scope === "observed_remaining_interval" ? t("web.cultivation_scope_remaining") : "—";
        row.append(node("th", "", role?.name || plan.role_id), node("td", "mono", track.id), node("td", "", track.current ?? "—"), node("td", "", track.target ?? "—"), node("td", "", scope)); body.append(row);
      }
      table.append(body); source.append(table, node("pre", "mono", JSON.stringify(result.source_tracks, null, 2))); output.append(source);
    }
    if (result.sources || result.missing || result.gaps || result.source_plans || result.owned_source) { const details = node("details", "panel-source"); details.append(node("summary", "", t("web.cultivation_sources")), node("pre", "mono", JSON.stringify({ sources: result.sources, source_plans: result.source_plans, missing: result.missing, gaps: result.gaps, owned_source: result.owned_source, owned_scope: result.owned_scope, owned_complete: result.owned_complete, owned_includes_bound: result.owned_includes_bound, owned_gaps: result.owned_gaps }, null, 2))); output.append(details); }
  }
  function render() {
    const { job, active, posting, error: jobError } = jobController.state(), disabled = !loaded || active || posting;
    for (const control of form.querySelectorAll("input,select,textarea,button")) control.disabled = disabled || control.dataset.orderDisabled === "true";
    for (const { plan, from, to, observedDetails } of editors) { from.setDisabled(disabled || plan.use_observed !== false); to.setDisabled(disabled); observedDetails.hidden = plan.use_observed === false; }
    add.disabled = disabled || !select.options.length || preferences.plans.length >= 32; run.disabled = disabled || !isReady() || !preferences.plans.length; cancel.disabled = !active || posting;
    phase.textContent = t(`web.optimize_status_${job.status || "idle"}`); notice.textContent = error || jobError || (job.kind === "cultivation" && job.status === "failed" ? job.message : "") || ""; notice.hidden = !notice.textContent; notice.classList.toggle("error", Boolean(error || jobError) || job.status === "failed"); renderResults();
  }
  form.onsubmit = async (event) => {
    event.preventDefault(); if (!isReady() || !form.reportValidity()) return;
    const plans = [];
    try {
      for (const { plan, from, to } of editors) {
        const target = to.value(); if (!Object.entries(target).some(([key, value]) => key !== "fork_id" && value != null)) throw new Error(t("web.cultivation_target_required"));
        plans.push({ id: plan.id, role_id: plan.role_id, use_observed: plan.use_observed !== false, from: plan.use_observed !== false ? plan.from || {} : from.value(), to: target });
      }
    } catch (failure) { error = failure.message; render(); return; }
    error = ""; await jobController.run("api/cultivation/plan", { plans, allow_crafting: preferences.allow_crafting });
  };
  jobController.subscribe(render);
  return { activate() { if (!loaded) initialize(); catalog(); render(); }, sync() { if (!workspace()?.account) return; if (!loaded) initialize(); catalog(); render(); } };
}
