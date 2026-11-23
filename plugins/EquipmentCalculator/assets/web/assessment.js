import { equipmentIcon, characterAvatar } from "./asset_images.js";

export function createAssessmentPanel({ mount, api, t, node, inventory, workspace, selectedItem, isReady, goEquipment, readRoles, saveRoles }) {
  let busy = false, result = null, error = "", selectedRoles = new Set(), rolesSignature = "", preferencesLoaded = false, itemVisual = "", resultVisual = null;
  const heading = node("div", "page-heading");
  heading.append(node("div")); heading.firstChild.append(node("h2", "", t("web.assessment_title")), node("p", "meta", t("web.assessment_hint")));
  const choose = node("button", "button quiet", t("web.assessment_choose")); choose.type = "button"; choose.onclick = goEquipment; heading.append(choose);
  const form = node("form", "assessment-form");
  const itemHeading = node("div", "assessment-item"), rolesLabel = node("label", "page-field");
  rolesLabel.append(node("span", "", t("web.assessment_roles")));
  const roles = node("select"); roles.multiple = true; roles.size = 5; roles.setAttribute("aria-label", t("web.assessment_roles"));
  roles.onchange = () => { selectedRoles = new Set([...roles.selectedOptions].map((option) => option.value)); saveRoles(JSON.stringify([...selectedRoles])); };
  rolesLabel.append(roles, node("small", "meta", t("web.assessment_roles_hint")));
  const run = node("button", "button primary", t("web.assessment_run")); run.type = "submit";
  form.append(itemHeading, rolesLabel, run);
  const message = node("p", "page-error"); message.setAttribute("role", "alert");
  const output = node("div", "assessment-results");
  mount.append(heading, form, message, output);

  function render() {
    if (!preferencesLoaded) {
      try { const saved = JSON.parse(readRoles() || "[]"); if (Array.isArray(saved)) selectedRoles = new Set(saved.filter((id) => typeof id === "string")); }
      catch { error = t("web.assessment_invalid_preferences"); }
      preferencesLoaded = true;
    }
    const item = selectedItem();
    const visual = JSON.stringify([item?.uid, item?.name, item?.item_id, item?.kind, item?.cells, item?.icon_asset_path, item?.plan_icon_asset_path, item?.icon_big_asset_path, item?.icon_small_asset_path]);
    if (itemVisual !== visual) {
      itemVisual = visual;
      const itemTitle = node("div", "assessment-item-title"); if (item) itemTitle.append(equipmentIcon(item, node)); itemTitle.append(node("strong", "", item ? item.name || item.item_id : t("web.choose_equipment"))); itemHeading.replaceChildren(itemTitle);
      if (item) itemHeading.append(node("span", "meta mono", `${item.uid.slot}:${item.uid.serial}`));
    }
    const available = (workspace()?.roles || []).filter((role) => !role.archived);
    const signature = JSON.stringify(available.map((role) => [role.id, role.name]));
    if (signature !== rolesSignature) {
      rolesSignature = signature;
      roles.replaceChildren(...available.map((role) => { const option = node("option", "", role.name); option.value = role.id; option.selected = selectedRoles.has(role.id); return option; }));
    }
    roles.disabled = busy; run.disabled = busy || !item || !isReady() || !available.length;
    run.textContent = t(busy ? "web.assessment_running" : "web.assessment_run");
    message.textContent = error; message.hidden = !error;
    if (!result) { if (resultVisual !== null) output.replaceChildren(); resultVisual = null; return; }
    const stale = result.stale || result.source_snapshot_id !== inventory()?.snapshot_id;
    const resultItem = inventory()?.items?.find((item) => item.uid.slot === result.equipment.slot && item.uid.serial === result.equipment.serial);
    const resultKey = JSON.stringify([result, stale, resultItem?.name, available.map((role) => [role.id, role.name])]);
    if (resultVisual === resultKey) return;
    resultVisual = resultKey; output.replaceChildren();
    output.append(node("h3", "", t("web.assessment_result_for", resultItem?.name || `${result.equipment.slot}:${result.equipment.serial}`)));
    if (stale) output.append(node("p", "draft-warning", t("web.assessment_stale")));
    output.append(node("p", "meta mono", t("web.snapshot", result.source_snapshot_id)));
    const table = node("table", "data-table"), head = node("thead"), header = node("tr");
    for (const key of ["web.character", "web.optimize_score", "web.optimize_grade", "web.assessment_breakdown"]) header.append(node("th", "", t(key)));
    head.append(header); table.append(head);
    const body = node("tbody");
    for (const score of result.results) {
      const role = available.find((value) => value.id === score.role_id), row = node("tr");
      const name = node("th"), identity = node("span", "table-role-identity"); identity.append(characterAvatar(role, node, "small"), node("span", "", role?.name || score.role_id)); name.append(identity);
      row.append(name, node("td", "", score.eligible && score.score != null ? score.score : "—"), node("td", "", score.eligible && score.grade != null ? score.grade : "—"));
      const detail = node("td", "meta");
      if (score.eligible) detail.textContent = t("web.assessment_scores", score.main_score ?? "—", score.sub_score ?? "—", score.priority_depth ?? "—");
      else detail.textContent = score.message || t("web.assessment_ineligible");
      row.append(detail); body.append(row);
    }
    table.append(body); output.append(table);
  }
  form.onsubmit = async (event) => {
    event.preventDefault();
    const item = selectedItem(); if (busy || !item || !isReady()) return;
    busy = true; error = ""; render();
    try { result = await api("api/assessment", { snapshot_id: inventory().snapshot_id, equipment: item.uid, ...(selectedRoles.size ? { role_ids: [...selectedRoles] } : {}) }); }
    catch (failure) { error = failure.message; }
    finally { busy = false; render(); }
  };
  return { activate: render, sync: render };
}
