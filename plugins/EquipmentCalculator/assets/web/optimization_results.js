import { equipmentIcon, characterAvatar } from "./asset_images.js";

const keyOf = (uid) => uid ? `${uid.slot}:${uid.serial}` : "";

export function renderPlanPreview({ plan, t, node, inventory, current, loadDraft, saveSoftwarePlan, roleName, title, compact = false, loaded = false, showCore = false }) {
  const number = (value) => value == null ? "—" : new Intl.NumberFormat(document.documentElement.lang || "en", { maximumFractionDigits: 2 }).format(value);
  const container = node("section", `optimization-plan${compact ? " compact" : ""}${loaded ? " is-loaded" : ""}`);
  const character = inventory()?.characters?.find((value) => keyOf(value.uid) === keyOf(plan.character));
  const equipment = inventory()?.items || [];
  const software = plan.native_assemblable === false || !plan.character;
  if (!compact) {
    const heading = node("div", "optimization-plan-heading");
    heading.append(characterAvatar(character || { id: plan.role_id, name: roleName?.(plan) }, node, "small"), node("h3", "", title || t("web.optimize_target", roleName?.(plan) || character?.name || plan.role_id || keyOf(plan.character)))); container.append(heading);
  } else if (!loaded) {
    const summary = node("div", "optimization-plan-summary");
    summary.append(node("span", "", title || character?.name || roleName?.(plan)), node("strong", "", `${number(plan.score)} · ${plan.grade ?? "—"}`));
    container.append(summary);
  }
  if (software) container.append(node("p", "draft-warning", t("web.optimize_software_only")));
  if (!compact) {
    const metrics = node("div", "optimization-metrics");
    for (const [label, value] of [["web.optimize_score", number(plan.score)], ["web.optimize_grade", plan.grade ?? "—"]]) {
      const metric = node("div"); metric.append(node("span", "field-label", t(label)), node("strong", "", value)); metrics.append(metric);
    }
    container.append(metrics);
  }
  if (!compact && showCore) {
    const core = plan.core ? equipment.find((item) => keyOf(item.uid) === keyOf(plan.core)) : null;
    const line = node("div", `optimization-core${core ? "" : " is-empty"}`);
    if (core) line.append(equipmentIcon(core, node, "small"));
    line.append(node("span", "visually-hidden", t("web.core")),
      node("span", "optimization-core-name", core ? core.name || t("web.core") : t(plan.core ? "web.missing_item" : "web.no_core")));
    container.append(line);
  }
  if (plan.equipment_complete === false) container.append(node("p", "draft-warning", t("web.optimize_missing_core")));
  const assignments = node("details", "optimizer-assignment-details"); assignments.append(node("summary", "", t("web.equipment_details")));
  const rows = node("div", "optimizer-assignments"); assignments.append(rows);
  for (const score of plan.assignment_scores || []) {
    const item = equipment.find((value) => keyOf(value.uid) === keyOf(score.equipment));
    const placement = plan.placements.find((entry) => keyOf(entry.equipment) === keyOf(score.equipment));
    const row = node("div", "optimizer-assignment"), copy = node("div", "draft-copy");
    copy.append(node("strong", "", item?.name || item?.item_id || keyOf(score.equipment)), node("small", "", placement ? t("web.position", placement.row, placement.column) : t("web.core")));
    row.append(equipmentIcon(item, node, "small"), copy, node("span", "optimizer-item-score", `${number(score.score)} · ${score.grade ?? "—"}`)); rows.append(row);
  }
  const load = node("button", "button primary", t(software ? "web.optimize_save_software" : "web.optimize_load_draft")); load.type = "button"; load.disabled = !current() || (software && !saveSoftwarePlan);
  load.onclick = () => { if (current()) { if (software) saveSoftwarePlan?.(plan); else loadDraft(plan); } };
  if (compact) { load.classList.remove("primary"); load.classList.add("secondary"); }
  load.hidden = loaded;
  container.append(assignments, load); return container;
}

export function renderSearchSummary({ result, source, stale, t, node }) {
  const container = node("div", "search-summary"), search = result.search || {};
  const exhaustive = search.exhaustive === true && result.enumeration_complete !== false;
  container.append(node("p", `optimizer-result-note${exhaustive ? "" : " incomplete"}`, t(exhaustive ? "web.search_exhaustive" : "web.search_incomplete")));
  if (stale) container.append(node("p", "optimizer-result-note incomplete", t("web.optimize_stale_preview")));
  const details = node("details", "search-details"); details.append(node("summary", "", t("web.calculation_details")), node("p", "optimizer-search-stats", t("web.optimize_search_budget", search.nodes ?? "—", search.elapsed_ms == null ? "—" : Math.round(search.elapsed_ms))));
  if (source) details.append(node("p", "meta mono", t("web.snapshot", source)));
  container.append(details);
  return container;
}
