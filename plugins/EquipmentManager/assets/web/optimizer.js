import { renderPlanPreview, renderSearchSummary } from "./optimization_results.js";
import { createNumberValidation } from "./number_validation.js";
import { createOptimizerConstraints } from "./optimizer_constraints.js";
import { generalOutputWeights, generalWeights, recommendedWeightsForRole, recommendedCoreForRole, resolveCorePreference, optimizationSetMode, getRecommendedWeightsCatalog, rolePreference, resolveOptimizationPreference, optimizationPreferenceFields } from "./weight_preferences.js";
import { propertyLabel, suitLabel } from "./equipment_display.js";
import { sameLoadout } from "./loadout_state.js";

const $ = (id) => document.getElementById(id);
const defaults = {
  weights: { ...generalOutputWeights }, main_weights: { ...generalOutputWeights }, shared_main_weights: true, preset: "general",
  set_effect_mode: "four_piece", require_core: true, core_suit_id: "", core_main_stats: [],
  priority_stats: [], priority_equal: false, priority_min_grade: "A",
  include_discarded: false, allow_borrow: true, exclude_selected: false,
  max_nodes: 1000000000, time_limit_ms: 120000, worker_count: 0, top_k: 1, keep_current: false, allow_missing_core: false, custom_role_id: "",
};
const keyOf = (uid) => uid ? `${uid.slot}:${uid.serial}` : "";
const recommendedSuitOption = "@recommended";

export function createOptimizer({ jobController, features, workspace, t, node, inventory, selectedCharacter, selectedUids,
  readPreference, savePreference, readRolePreferences, saveRolePreferences, currentRole, api, reloadWorkspace, canRun, loadDraft, saveSoftwarePlan, previewPlan, refreshRecommendedWeights }) {
  let preferences = structuredClone(defaults);
  let loaded = false;
  let catalogSignature = "", localError = "", preferenceError = "";
  let renderedOutputKey = "";
  let contextId = "", rolePreferences = {}, preset = "general", autoPreviewId = "", ownJobId = "", remembering = false, preferenceMessage = "";
  let optionJobId = "", selectedOption = 0, shownOptionKey = "";
  const validateNumbers = createNumberValidation($("optimizer-form"), t, "#opt-node-limit, #opt-time-limit, #opt-worker-count, #opt-top-k");

  const stateItems = () => inventory()?.items || [];
  const targetRole = () => preferences.custom_role_id ? (workspace()?.roles || []).find((role) => role.id === preferences.custom_role_id && role.official === false) : selectedCharacter();
  const constraints = createOptimizerConstraints({ mount: $("optimizer-constraints"), t, node, inventory, preferences: () => preferences, changed });

  function changed(keepPreset = false) {
    if (!loaded) return;
    localError = "";
    preferenceError = "";
    if (!keepPreset) preset = "custom";
    preferences.preset = preset;
    preferences.preference_role_id = contextId;
    savePreference(JSON.stringify(preferences));
    if (contextId) { rolePreferences[contextId] = { ...preferences, preset }; saveRolePreferences(JSON.stringify(rolePreferences)); }
    render();
  }

  function selectOptions(element, entries, selected) {
    if (JSON.stringify([...element.options].map((option) => [option.value, option.textContent])) !== JSON.stringify(entries)) element.replaceChildren(...entries.map(([value, label]) => {
      const option = node("option", "", label);
      option.value = value;
      option.selected = value === selected;
      return option;
    }));
    for (const option of element.options) option.selected = option.value === selected;
  }

  function catalog() {
    const stats = new Map(), subStats = new Set(), mains = new Set(), suits = new Map();
    for (const item of stateItems()) {
      for (const stat of [...(item.attributes || []), ...(item.sub_stats || [])]) {
        if (typeof stat.id !== "string" || !stat.id) continue;
        stats.set(stat.id, propertyLabel(t, stat));
        subStats.add(stat.id);
      }
      if (item.main_stat?.id) {
        stats.set(item.main_stat.id, propertyLabel(t, item.main_stat));
        mains.add(item.main_stat.id);
      }
      if (item.kind === "core" && item.suit_id) suits.set(item.suit_id, suitLabel(t, item));
    }
    for (const id of [...Object.keys(preferences.weights), ...Object.keys(preferences.main_weights), ...preferences.priority_stats, ...preferences.core_main_stats]) if (!stats.has(id)) stats.set(id, propertyLabel(t, id));
    for (const id of preferences.priority_stats) subStats.add(id);
    for (const id of preferences.core_main_stats) mains.add(id);
    const role = preferences.custom_role_id ? workspace()?.roles?.find((value) => value.id === preferences.custom_role_id) : currentRole();
    const recommended = recommendedCoreForRole(role);
    if (recommended) suits.set(recommended.suit_id, suitLabel(t, { ...recommended, kind: "core" }));
    if (preferences.core_suit_id && !suits.has(preferences.core_suit_id)) suits.set(preferences.core_suit_id, t("web.unknown_suit"));
    const suitOptions = [
      ...(role && role.official !== false ? [[recommendedSuitOption, recommended ? t("web.optimize_recommended_suit", suitLabel(t, { ...recommended, kind: "core" })) : t("web.optimize_recommended_suit_pending")]] : []),
      ["", t("web.optimize_any_suit")], ...suits,
    ];
    const selectedSuit = preferences.core_suit_source === "recommended" ? recommendedSuitOption : preferences.core_suit_id;
    const entries = [...stats].sort(([, leftName], [, rightName]) => leftName.localeCompare(rightName, document.documentElement.lang || "en"));
    const signature = JSON.stringify([entries, [...subStats].sort(), [...mains].sort(), suitOptions, selectedSuit]);
    if (signature === catalogSignature) return;
    catalogSignature = signature;
    const rows = entries.map(([id, name]) => {
      const row = node("tr");
      const heading = node("th");
      heading.scope = "row";
      heading.append(node("span", "", name));
      row.append(heading);
      for (const [field, label] of [["weights", "web.optimize_sub_weight"], ["main_weights", "web.optimize_main_weight"]]) {
        const cell = node("td");
        const input = node("input");
        input.type = "number"; input.min = "0"; input.max = "1"; input.step = "any";
        input.value = String(preferences[field][id] ?? 0);
        input.dataset.weightField = field; input.dataset.stat = id;
        input.setAttribute("aria-label", `${name} · ${t(label)}`);
        input.oninput = () => {
          const value = input.value === "" ? 0 : input.valueAsNumber;
          const valid = Number.isFinite(value) && value >= 0 && value <= 1;
          input.setCustomValidity(valid ? "" : t("web.optimize_invalid_weight"));
          if (valid) { preferences[field][id] = value; changed(); } else render();
        };
        cell.append(input); row.append(cell);
      }
      return row;
    });
    if (!rows.length) {
      const row = node("tr"), cell = node("td", "optimizer-empty", t("web.optimize_no_properties"));
      cell.colSpan = 3; row.append(cell); rows.push(row);
    }
    $("weight-rows").replaceChildren(...rows);
    selectOptions($("opt-core-suit"), suitOptions, selectedSuit);
    selectOptions($("opt-core-main"), [["", t("web.optimize_any_main")], ...[...mains].sort().map((id) => [id, stats.get(id) || id])], "");
    for (let index = 0; index < 4; index++) selectOptions($(`opt-priority-${index + 1}`), [["", t("web.optimize_no_priority")], ...[...subStats].sort().map((id) => [id, stats.get(id) || id])], preferences.priority_stats[index] || "");
  }

  function renderCoreMainStats(disabled) {
    const select = $("opt-core-main"), selected = $("opt-core-main-selected"), clear = $("opt-core-main-clear");
    select.disabled = disabled;
    select.options[0].textContent = t(preferences.core_main_stats.length ? "web.optimize_add_main" : "web.optimize_any_main");
    for (const option of select.options) option.disabled = !option.value || preferences.core_main_stats.includes(option.value);
    const names = new Map([...select.options].map((option) => [option.value, option.textContent]));
    const entries = preferences.core_main_stats.map((id) => [id, names.get(id) || propertyLabel(t, id)]);
    const signature = JSON.stringify(entries);
    if (selected.dataset.signature !== signature) {
      selected.dataset.signature = signature;
      selected.replaceChildren(...entries.map(([id, name]) => {
        const remove = node("button", "button secondary optimizer-main-stat"); remove.type = "button";
        remove.setAttribute("aria-label", t("web.optimize_remove_main", name));
        const icon = node("span", "", "×"); icon.setAttribute("aria-hidden", "true");
        remove.append(node("span", "", name), icon);
        remove.onclick = () => { preferences.core_main_stats = preferences.core_main_stats.filter((value) => value !== id); changed(); select.focus(); };
        return remove;
      }));
    }
    selected.hidden = !entries.length;
    clear.hidden = !entries.length;
    clear.disabled = disabled;
    for (const button of selected.querySelectorAll("button")) button.disabled = disabled;
  }

  function initialize(saved = readPreference()) {
    if (saved) {
      try {
        const value = JSON.parse(saved);
        const validWeights = (weights) => weights && typeof weights === "object" && !Array.isArray(weights) && Object.values(weights).every((weight) => Number.isFinite(weight) && weight >= 0 && weight <= 1);
        const ids = (entries) => Array.isArray(entries) && entries.every((id) => typeof id === "string" && id.length > 0);
        const candidate = { ...preferences, ...value };
        const validFilters = !Object.hasOwn(candidate, "filters") || (candidate.filters && typeof candidate.filters === "object" && !Array.isArray(candidate.filters) && Object.values(candidate.filters).every((filter) => filter && typeof filter === "object" && !Array.isArray(filter) && ["qualities", "main_allow", "main_blacklist", "sub_blacklist"].every((field) => !Object.hasOwn(filter, field) || ids(filter[field]))));
        const validLimits = !Object.hasOwn(candidate, "property_limits") || (Array.isArray(candidate.property_limits) && candidate.property_limits.every((limit) => limit && typeof limit.id === "string" && ["minimum", "maximum"].every((field) => !Object.hasOwn(limit, field) || Number.isFinite(limit[field]))));
        if (!value || typeof value !== "object" || Array.isArray(value) ||
            !validFilters || !validLimits || !Number.isInteger(candidate.worker_count) || candidate.worker_count < 0 || candidate.worker_count > 32 ||
            (value.preset != null && !["recommended", "general", "role", "custom"].includes(value.preset)) ||
            !validWeights(candidate.weights) || !validWeights(candidate.main_weights) ||
            !ids(candidate.priority_stats) || candidate.priority_stats.length > 4 || !ids(candidate.core_main_stats) ||
            typeof candidate.core_suit_id !== "string" || !["four_piece", "two_piece", "none"].includes(candidate.set_effect_mode) ||
            (candidate.core_suit_source != null && !["manual", "recommended"].includes(candidate.core_suit_source)) ||
            !["D", "C", "B", "A", "S", "SS", "SSS", "ACE"].includes(candidate.priority_min_grade) ||
            ["shared_main_weights", "require_core", "priority_equal", "include_discarded", "allow_borrow", "exclude_selected", "keep_current", "allow_missing_core"].some((key) => typeof candidate[key] !== "boolean")) throw new Error();
        let restoredPreset = value.preset ?? (Object.hasOwn(value, "weights") || Object.hasOwn(value, "main_weights") ? "custom" : "general");
        const general = generalWeights().weights;
        const matchesGeneral = (weights) => Object.entries(general).every(([id, weight]) => weights[id] === weight) &&
          Object.entries(weights).every(([id, weight]) => weight === 0 || general[id] === weight);
        const hasSubWeights = Object.keys(value.weights || {}).length > 0, hasMainWeights = Object.keys(value.main_weights || {}).length > 0;
        // Older global saves retained "general" after the user edited weights.
        // Keep those explicit values; only an actually empty General is repaired.
        if (restoredPreset === "general" && (hasSubWeights || hasMainWeights) &&
            (value.shared_main_weights === false || hasSubWeights && !matchesGeneral(value.weights) || hasMainWeights && !matchesGeneral(value.main_weights))) restoredPreset = "custom";
        preferences = restoredPreset === "general" ? resolveOptimizationPreference({}, { ...candidate, preset: "general" }) :
          { ...candidate, weights: value.weights ?? {}, main_weights: value.main_weights ?? {}, preset: restoredPreset };
      } catch { preferenceError = t("web.optimize_invalid_preferences"); }
    }
    preset = preferences.preset;
    for (const [id, field] of [["opt-require-core", "require_core"], ["opt-shared-main", "shared_main_weights"], ["opt-priority-equal", "priority_equal"], ["opt-discarded", "include_discarded"], ["opt-borrow", "allow_borrow"], ["opt-exclude-selected", "exclude_selected"]]) $(id).checked = preferences[field];
    $("opt-set-mode").value = preferences.set_effect_mode;
    $("opt-priority-grade").value = preferences.priority_min_grade;
    $("opt-node-limit").value = preferences.max_nodes;
    $("opt-time-limit").value = preferences.time_limit_ms / 1000;
    $("opt-worker-count").value = preferences.worker_count;
    $("opt-top-k").value = preferences.top_k;
    loaded = true;
  }

  function selectContext(force = false) {
    const role = preferences.custom_role_id ? workspace()?.roles?.find((value) => value.id === preferences.custom_role_id) : currentRole();
    if (!role) return;
    if (!force && contextId === role.id) { preferences = resolveCorePreference(role, preferences); return; }
    try { rolePreferences = JSON.parse(readRolePreferences() || "{}"); if (!rolePreferences || typeof rolePreferences !== "object" || Array.isArray(rolePreferences)) throw new Error(); }
    catch { preferenceError = t("web.optimize_invalid_preferences"); return; }
    const previous = contextId;
    contextId = role.id; preferenceMessage = ""; catalogSignature = "";
    const stored = rolePreferences[role.id];
    // An anonymous global preference cannot establish which character owns it.
    const migrate = !previous && !stored && preferences.preference_role_id === role.id;
    const next = resolveOptimizationPreference(role, stored, migrate ? preferences : null);
    preferences = { ...structuredClone(defaults), ...structuredClone(next), custom_role_id: preferences.custom_role_id };
    preset = next.preset || (migrate ? "custom" : "general");
    initialize(JSON.stringify(preferences)); catalog(); constraints.reload();
    return !preferenceError;
  }

  function request() {
    const character = targetRole(), software = Boolean(preferences.custom_role_id);
    const result = {
      snapshot_id: inventory().snapshot_id, character: software ? null : character.uid,
      ...(software ? { role_id: character.id, allowed_cells: character.allowed_cells } : {}),
      ...optimizationPreferenceFields(preferences, stateItems(), software ? null : character.uid),
      max_nodes: Number($("opt-node-limit").value), time_limit_ms: Math.round(Number($("opt-time-limit").value) * 1000), worker_count: Number($("opt-worker-count").value),
    };
    if (preferences.exclude_selected) result.exclude_uids = selectedUids();
    if (features().optimize_top_k === true) result.top_k = Number($("opt-top-k").value);
    return result;
  }

  function resultIsCurrent(plan) {
    const { job, applied } = jobController.state();
    return !applied && job.status === "completed" && plan.valid === true && !job.stale &&
      job.source_snapshot_id === inventory()?.snapshot_id;
  }

  function renderResult() {
    const container = $("optimization-result");
    const { job, applied } = jobController.state();
    const result = job.result;
    container.hidden = applied || !["single", "top_k"].includes(job.kind) || job.status !== "completed" || !result || (!Array.isArray(result.placements) && !Array.isArray(result.options));
    if (container.hidden) return;
    if (optionJobId !== job.id) { optionJobId = job.id; selectedOption = 0; shownOptionKey = ""; }
    const plans = result.options || (result.valid ? [result] : []);
    selectedOption = Math.min(selectedOption, Math.max(0, plans.length - 1));
    const shown = previewPlan?.(), loaded = plans.map((plan) => sameLoadout(plan, shown));
    if (shownOptionKey !== loaded.join()) {
      shownOptionKey = loaded.join();
      const displayed = loaded.indexOf(true);
      if (displayed >= 0) selectedOption = displayed;
    }
    const key = `${job.id || ""}|${job.status}|${job.stale}|${inventory()?.snapshot_id}|${Boolean(canRun())}|${selectedOption}|${loaded.join()}`;
    if (key === renderedOutputKey) return; renderedOutputKey = key;
    const stale = job.stale || job.source_snapshot_id !== inventory()?.snapshot_id;
    container.replaceChildren(renderSearchSummary({ result, source: job.source_snapshot_id || result.snapshot_id, stale, t, node }));
    if (!result.valid) container.append(node("p", "optimizer-result-note incomplete", t(result.reason === "budget_exhausted" ? "web.optimize_no_complete_layout" : "web.optimize_no_solution")));
    if (plans.length > 1) {
      const label = node("label", "page-field optimizer-alternative-picker"), select = node("select");
      label.append(node("span", "", t("web.optimize_alternatives_label")), select);
      for (const [index, plan] of plans.entries()) {
        const score = plan.score == null ? "—" : new Intl.NumberFormat(document.documentElement.lang || "en", { maximumFractionDigits: 2 }).format(plan.score);
        const option = node("option", "", `${t("web.optimize_option", index + 1)} · ${score} · ${plan.grade ?? "—"}`);
        option.value = String(index); option.selected = index === selectedOption; select.append(option);
      }
      select.onchange = () => {
        selectedOption = Number(select.value); renderedOutputKey = "";
        const plan = plans[selectedOption];
        if (resultIsCurrent(plan) && plan.native_assemblable !== false && plan.character && keyOf(plan.character) === keyOf(selectedCharacter()?.uid))
          loadDraft({ ...plan, snapshot_id: job.source_snapshot_id });
        else renderResult();
      };
      container.append(label);
    }
    if (plans.length) {
      const plan = plans[selectedOption];
      container.append(renderPlanPreview({ plan, t, node, inventory, current: () => resultIsCurrent(plan),
        loadDraft: (value) => loadDraft({ ...value, snapshot_id: job.source_snapshot_id }), saveSoftwarePlan: (value) => saveSoftwarePlan({ ...value, snapshot_id: job.source_snapshot_id }),
        roleName: (plan) => workspace()?.roles?.find((role) => role.id === plan.role_id)?.name,
        compact: Boolean(plan.character && plan.native_assemblable !== false), loaded: loaded[selectedOption],
        ...(result.options ? { title: t("web.optimize_option", selectedOption + 1) } : {}) }));
    }
    const first = result.options?.[0] || result;
    if (job.id === ownJobId && autoPreviewId !== job.id && first.valid === true && !stale && first.native_assemblable !== false && first.character && keyOf(first.character) === keyOf(selectedCharacter()?.uid)) {
      autoPreviewId = job.id; queueMicrotask(() => loadDraft({ ...first, snapshot_id: job.source_snapshot_id }));
    }
  }

  function render() {
    const { job, posting, active, cancelling, error, applied } = jobController.state();
    const role = preferences.custom_role_id ? workspace()?.roles?.find((value) => value.id === preferences.custom_role_id) : currentRole();
    if (role) preferences = resolveCorePreference(role, preferences);
    catalog();
    const target = targetRole();
    const custom = (workspace()?.roles || []).filter((role) => role.official === false && !role.archived && role.allowed_cells?.length === 20);
    const targets = [["", t("web.optimize_current_native")], ...custom.map((role) => [role.id, role.name])];
    if (JSON.stringify([...$("opt-role-target").options].map((option) => [option.value, option.textContent])) !== JSON.stringify(targets)) selectOptions($("opt-role-target"), targets, preferences.custom_role_id);
    $("opt-role-target").value = preferences.custom_role_id;
    $("optimizer-software-hint").hidden = !preferences.custom_role_id;
    const hasRecommended = Boolean(recommendedWeightsForRole(role));
    const choices = [
      ...(hasRecommended ? [["recommended", t("web.preference_recommended")]] : []),
      ["general", t("web.preference_general")],
      ...(role && Object.hasOwn(role, "weights") ? [["role", t("web.preference_role")]] : []),
      ["custom", t("web.preference_custom")],
    ];
    selectOptions($("opt-preset"), choices, preset);
    const weightCatalog = getRecommendedWeightsCatalog();
    const statusText = weightCatalog.status === "fetching" ? t("web.recommended_weights_fetching") :
      weightCatalog.origin === "builtin" ? t("web.recommended_weights_builtin") :
      t("web.weights_attribution");
    $("recommended-weights-status").textContent = statusText;
    $("recommended-weights-status").title = weightCatalog.remote_error || statusText;
    $("refresh-recommended-weights").hidden = features().recommended_weights !== true || !refreshRecommendedWeights;
    $("refresh-recommended-weights").disabled = !loaded || active || posting || weightCatalog.status === "fetching";
    const names = new Map(); for (const item of stateItems()) for (const stat of [...(item.sub_stats || []), ...(item.main_stat ? [item.main_stat] : [])]) names.set(stat.id, propertyLabel(t, stat));
    const preferred = Object.entries(preferences.weights).filter(([, value]) => value > 0).sort(([, left], [, right]) => right - left).slice(0, 6).map(([id]) => names.get(id) || propertyLabel(t, id));
    const summaryEl = $("preset-summary");
    const summaryKey = preferred.length ? preferred.join("\0") : "";
    if (summaryEl.dataset.summaryKey !== summaryKey) {
      summaryEl.dataset.summaryKey = summaryKey;
      if (preferred.length) summaryEl.replaceChildren(...preferred.map((label) => node("span", "preference-stat", label)));
      else summaryEl.textContent = t("web.preference_empty");
    }
    $("save-role-preference").disabled = !role || remembering || active || posting;
    $("optimization-phase").textContent = t(cancelling ? "web.search_progress_cancelling" : applied ? "web.operation_confirmed" : `web.optimize_status_${job.status || "idle"}`);
    $("optimization-phase").dataset.status = applied ? "confirmed" : job.status || "idle";
    const globalProgressVisible = !$("calculation-progress").hidden;
    $("optimization-phase").hidden = !active || globalProgressVisible;
    const missingRecommendedSuit = preferences.require_core && preferences.core_suit_source === "recommended" && !preferences.core_suit_id;
    const suitMessage = missingRecommendedSuit ? t("web.optimize_recommended_suit_missing", role?.name || t("web.character")) : "";
    $("optimization-message").textContent = preferenceError || localError || error || suitMessage || preferenceMessage || (["failed", "cancelled"].includes(job.status) ? job.message || t(`algorithm.${job.status}`) : "");
    $("optimization-message").hidden = !$("optimization-message").textContent;
    $("optimization-message").classList.toggle("error", Boolean(preferenceError || localError || error) || job.status === "failed");
    $("optimizer-exclude-label").textContent = t("web.optimize_exclude_selected", selectedUids().length);
    const locked = !loaded || active || posting;
    for (const control of $("optimizer-form").querySelectorAll("input,select")) control.disabled = locked;
    $("opt-set-mode").value = optimizationSetMode(preferences);
    $("opt-set-mode").disabled = true;
    $("opt-core-suit").disabled = locked || !preferences.require_core;
    renderCoreMainStats(locked || !preferences.require_core);
    const needsVerifiedSuit = preferences.allow_missing_core && preferences.require_core && preferences.set_effect_mode !== "none";
    $("opt-core-suit").setCustomValidity(suitMessage || (needsVerifiedSuit && !stateItems().some((item) => item.kind === "core" && item.suit_id === preferences.core_suit_id && item.required_shapes_complete === true) ? t("web.constraints_missing_core_suit") : ""));
    for (const input of $("weight-rows").querySelectorAll("[data-weight-field=main_weights]")) {
      input.disabled = locked || preferences.shared_main_weights;
      if (preferences.shared_main_weights) input.value = String(preferences.weights[input.dataset.stat] ?? 0);
    }
    $("opt-top-k-field").hidden = features().optimize_top_k !== true;
    $("opt-top-k").disabled = locked || features().optimize_top_k !== true;
    $("opt-increase-budget").disabled = locked;
    $("optimize-start").disabled = !loaded || !canRun() || !target || active || posting || Boolean(preferenceError) || !Object.values(preferences.weights).some((value) => value > 0) || !$("optimizer-form").checkValidity();
    const hasResult = ["single", "top_k"].includes(job.kind) && job.status === "completed" && (job.result?.valid || job.result?.options?.some((plan) => plan.valid));
    $("optimize-start").textContent = active ? t("web.calculating") : t(hasResult ? "web.recalculate_for" : "web.calculate_for", target?.name || t("web.character"));
    const showingPreview = Boolean(previewPlan?.());
    $("optimize-start").classList.toggle("primary", !showingPreview); $("optimize-start").classList.toggle("secondary", showingPreview);
    $("optimize-cancel").disabled = !active || posting || cancelling; $("optimize-cancel").hidden = !active || globalProgressVisible;
    $("optimize-cancel").textContent = t(cancelling ? "web.search_progress_cancelling" : "web.optimize_cancel");
    constraints.render(locked, Boolean(preferences.custom_role_id));
    validateNumbers();
    renderResult();
  }

  for (const [id, field] of [["opt-require-core", "require_core"], ["opt-shared-main", "shared_main_weights"], ["opt-priority-equal", "priority_equal"], ["opt-discarded", "include_discarded"], ["opt-borrow", "allow_borrow"], ["opt-exclude-selected", "exclude_selected"]]) $(id).onchange = () => {
    preferences[field] = $(id).checked;
    if (field === "require_core") preferences.set_effect_mode = optimizationSetMode(preferences);
    if (field === "shared_main_weights" && !preferences.shared_main_weights) for (const input of $("weight-rows").querySelectorAll("[data-weight-field=main_weights]")) input.value = String(preferences.main_weights[input.dataset.stat] ?? 0);
    changed();
  };
  $("opt-priority-grade").onchange = () => { preferences.priority_min_grade = $("opt-priority-grade").value; changed(); };
  $("opt-core-suit").onchange = () => {
    preferences.core_suit_source = $("opt-core-suit").value === recommendedSuitOption ? "recommended" : "manual";
    preferences.core_suit_id = preferences.core_suit_source === "manual" ? $("opt-core-suit").value : "";
    changed();
  };
  $("opt-core-main").onchange = () => {
    const select = $("opt-core-main");
    if (!select.value) return;
    preferences.core_main_stats.push(select.value); select.value = ""; changed();
  };
  $("opt-core-main-clear").onclick = () => { preferences.core_main_stats = []; changed(); $("opt-core-main").focus(); };
  $("opt-role-target").onchange = () => { preferences.custom_role_id = $("opt-role-target").value; selectContext(true); changed(true); };
  $("opt-preset").onchange = () => {
    preset = $("opt-preset").value;
    const role = preferences.custom_role_id ? workspace()?.roles?.find((value) => value.id === preferences.custom_role_id) : currentRole();
    if (preset === "recommended") preferences = resolveOptimizationPreference(role, { ...preferences, preset: "recommended" });
    else if (preset === "general") {
      const general = generalWeights();
      Object.assign(preferences, { weights: general.weights, main_weights: general.main_weights, shared_main_weights: JSON.stringify(general.weights) === JSON.stringify(general.main_weights) });
    } else if (preset === "role") preferences = resolveOptimizationPreference(role, { ...preferences, preset: "role" });
    else $("optimizer-advanced").open = true;
    preferences.preset = preset;
    initialize(JSON.stringify(preferences)); catalogSignature = ""; catalog(); constraints.reload(); changed(true);
  };
  $("refresh-recommended-weights").onclick = () => { if (refreshRecommendedWeights) refreshRecommendedWeights(); };
  $("save-role-preference").onclick = async () => {
    const role = preferences.custom_role_id ? workspace()?.roles?.find((value) => value.id === preferences.custom_role_id) : currentRole();
    if (!role || remembering) return; remembering = true; localError = ""; render();
    try { await api("api/roles", { action: "update", id: role.id, role: rolePreference({ ...preferences, main_weights: preferences.shared_main_weights ? preferences.weights : preferences.main_weights }) }); await reloadWorkspace(); preferenceMessage = t("web.preference_updated"); }
    catch (failure) { localError = failure.message; }
    finally { remembering = false; render(); }
  };
  for (let index = 1; index <= 4; index++) $(`opt-priority-${index}`).onchange = () => { preferences.priority_stats = Array.from({ length: 4 }, (_, position) => $(`opt-priority-${position + 1}`).value).filter(Boolean); changed(); };
  for (const [id, field] of [["opt-node-limit", "max_nodes"], ["opt-time-limit", "time_limit_ms"], ["opt-worker-count", "worker_count"], ["opt-top-k", "top_k"]]) $(id).oninput = () => {
    if ($(id).checkValidity()) { preferences[field] = Math.round($(id).valueAsNumber * (field === "time_limit_ms" ? 1000 : 1)); changed(true); } else render();
  };
  $("opt-increase-budget").onclick = () => {
    const { active, posting } = jobController.state(); if (active || posting) return;
    preferences.max_nodes = Math.max(preferences.max_nodes, defaults.max_nodes);
    preferences.time_limit_ms = Math.max(preferences.time_limit_ms, defaults.time_limit_ms);
    $("opt-node-limit").value = preferences.max_nodes; $("opt-time-limit").value = preferences.time_limit_ms / 1000;
    changed(true);
  };
  $("optimizer-form").onsubmit = async (event) => {
    event.preventDefault();
    const { active, posting } = jobController.state();
    if (!loaded || !canRun() || !targetRole() || active || posting || preferenceError || !$("optimizer-form").reportValidity()) return;
    if (new Set(preferences.priority_stats).size !== preferences.priority_stats.length) { localError = t("web.optimize_priority_unique"); render(); return; }
    localError = ""; const accepted = await jobController.run("api/optimize", request()); ownJobId = accepted?.id || ""; renderedOutputKey = ""; render();
  };
  $("optimize-cancel").onclick = async () => {
    await jobController.cancel();
  };

  jobController.subscribe(render);
  return {
    sync() { if (!loaded) initialize(); selectContext(); catalog(); render(); jobController.refresh(); },
    syncRecommendedWeights() {
      if (!loaded) return;
      const role = preferences.custom_role_id ? workspace()?.roles?.find((value) => value.id === preferences.custom_role_id) : currentRole();
      if (role && (preset === "recommended" || preset === "general" || (!rolePreferences[role.id] && !Object.values(role.weights || {}).some((weight) => weight > 0)))) {
        const stored = rolePreferences[role.id];
        const next = resolveOptimizationPreference(role, stored ? { ...preferences, preset } : undefined);
        preferences = { ...structuredClone(defaults), ...structuredClone(next), custom_role_id: preferences.custom_role_id };
        preset = next.preset || "general";
        initialize(JSON.stringify(preferences)); catalogSignature = ""; catalog(); constraints.reload();
      }
      render();
    },
    reloadPreferences() { preferences = structuredClone(defaults); loaded = false; catalogSignature = ""; initialize(); catalog(); constraints.reload(); render(); },
    selectContext() { preferences.custom_role_id = ""; selectContext(); render(); },
    selectRole(role) { preferences.custom_role_id = role.official === false ? role.id : ""; if (selectContext(true)) changed(true); else render(); },
    render,
  };
}
