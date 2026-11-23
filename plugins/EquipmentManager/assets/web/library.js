import { equipmentIcon } from "./asset_images.js";

export function createLibraryPanel({ mount, api, t, node, workspace, inventory, isReady, currentRole,
  readRole, saveRole, getDraft, loadDraft, reloadWorkspace }) {
  let roleId = "", slots = [], selected = "", history = null, busy = false, error = "", activated = false;
  let incomingPreview = null, requestedRole = "";
  const title = node("div", "page-heading");
  title.append(node("div", "", ""));
  title.firstChild.append(node("h2", "", t("web.library_title")), node("p", "meta", t("web.library_hint")));
  const refresh = node("button", "button quiet", t("web.refresh")); refresh.type = "button"; refresh.onclick = () => loadSlots(); title.append(refresh);
  const toolbar = node("div", "page-toolbar");
  const roleSelect = node("select"); roleSelect.setAttribute("aria-label", t("web.character"));
  roleSelect.onchange = () => { roleId = roleSelect.value; saveRole(roleId); history = null; loadSlots(); };
  toolbar.append(roleSelect);
  const saveStatus = node("span", "meta"); toolbar.append(saveStatus);
  const notice = node("p", "page-error"); notice.setAttribute("role", "alert");
  const body = node("div", "library-layout");
  mount.append(title, toolbar, notice, body);

  function roles() { return (workspace()?.roles || []).filter((role) => !role.archived); }
  function selectedSlot() { return slots.find((slot) => slot.id === selected); }
  function selectedRole() { return roles().find((role) => role.id === roleId); }
  function ready() { return isReady(); }
  function draftForRole() { return incomingPreview?.role_id === roleId ? incomingPreview : getDraft(); }
  function planForRole(plan) {
    const character = selectedRole()?.character;
    if (selectedRole()?.official === false) return plan?.character == null && plan?.role_id === roleId;
    return plan && character && plan.character?.slot === character.slot && plan.character?.serial === character.serial;
  }
  function slotName(slot) { return slot.name || t("web.library_primary"); }
  function button(label, action, disabled = false, style = "quiet") {
    const result = node("button", `button ${style}`, t(label)); result.type = "button"; result.disabled = busy || disabled; result.onclick = action; return result;
  }
  function scoreSummary(value) {
    return value.score == null || value.grade == null ? t("web.library_unscored") : t("web.library_score", value.score, value.grade);
  }
  function renderRoles() {
    const available = roles();
    if (!available.some((role) => role.id === roleId)) roleId = available.some((role) => role.id === readRole()) ? readRole() : available.some((role) => role.id === currentRole()) ? currentRole() : available[0]?.id || "";
    const values = available.map((role) => [role.id, role.name]);
    if (JSON.stringify([...roleSelect.options].map((value) => [value.value, value.textContent])) !== JSON.stringify(values)) {
      roleSelect.replaceChildren(...values.map(([id, label]) => { const option = node("option", "", label); option.value = id; return option; }));
    }
    roleSelect.value = roleId; roleSelect.disabled = busy || !available.length;
    const status = workspace()?.save_status;
    saveStatus.textContent = status === "saved" ? t("web.saved") : status === "failed" ? t("web.save_failed") : status === "saving" ? t("web.saving") : "";
  }
  function render() {
    renderRoles();
    refresh.disabled = busy;
    notice.textContent = error; notice.hidden = !error;
    body.replaceChildren();
    if (!roleId) { body.append(node("div", "empty", t("web.library_no_roles"))); return; }
    const list = node("div", "library-slots");
    for (const slot of slots.filter((value) => !value.archived)) {
      const row = node("button", `slot-choice${slot.id === selected ? " active" : ""}`); row.type = "button"; row.disabled = busy;
      row.append(node("strong", "", slotName(slot)), node("span", "meta", slot.locked ? t("web.library_locked") : scoreSummary(slot)));
      row.onclick = () => mutate({ action: "select", role_id: roleId, slot_id: slot.id });
      list.append(row);
    }
    const create = node("form", "inline-create");
    const name = node("input"); name.required = true; name.maxLength = 128; name.placeholder = t("web.library_new_name"); name.setAttribute("aria-label", t("web.library_new_name"));
    const createButton = node("button", "button secondary", t("web.library_create")); createButton.type = "submit"; createButton.disabled = busy || slots.filter((slot) => !slot.archived).length >= 3;
    create.onsubmit = (event) => { event.preventDefault(); if (create.reportValidity()) mutate({ action: "create", role_id: roleId, name: name.value }); };
    create.append(name, createButton);
    if (slots.filter((value) => !value.archived).length < 3) {
      const creation = node("details", "library-slot-create"); creation.append(node("summary", "", t("web.library_create")), create); list.append(creation);
    } else list.append(node("p", "meta", t("web.library_slot_limit")));
    const detail = node("div", "library-detail");
    const slot = selectedSlot();
    if (!slot) detail.append(node("div", "empty", t("web.library_choose_slot")));
    else {
      const heading = node("div", "library-detail-heading");
      const lock = button(slot.locked ? "web.library_unlock" : "web.library_lock", () => mutate({ action: "lock", role_id: roleId, slot_id: slot.id, locked: !slot.locked, expected_revision: slot.current_revision }), !slot.plan || (!slot.locked && !slot.plan.core && !slot.plan.placements.length));
      lock.setAttribute("aria-pressed", String(slot.locked)); lock.classList.add("slot-lock-toggle");
      heading.append(node("h3", "", slotName(slot)), lock); detail.append(heading);
      detail.append(node("p", "meta", scoreSummary(slot)));
      const rename = node("form", "inline-create");
      const renameInput = node("input"); renameInput.value = slot.name; renameInput.required = true; renameInput.maxLength = 128; renameInput.setAttribute("aria-label", t("web.library_slot_name"));
      renameInput.disabled = busy || slot.locked;
      const renameButton = node("button", "button quiet", t("web.library_rename")); renameButton.type = "submit"; renameButton.disabled = busy || slot.locked;
      rename.onsubmit = (event) => { event.preventDefault(); if (rename.reportValidity()) mutate({ action: "rename", role_id: roleId, slot_id: slot.id, name: renameInput.value, expected_revision: slot.current_revision }); };
      rename.append(renameInput, renameButton);
      const draft = draftForRole(), preview = incomingPreview?.role_id === roleId;
      const missingCount = (slot.plan?.placeholders?.length || 0) + (slot.plan?.missing_core ? 1 : 0);
      const actions = node("div", "page-actions");
      actions.append(
        button(preview ? "web.library_save_preview" : "web.library_save_draft", () => {
          const current = draftForRole();
          mutate({ action: "save", role_id: roleId, slot_id: slot.id, plan: { character: current.character, ...(current.role_id ? { role_id: current.role_id } : {}), core: current.core, placements: current.placements }, source_snapshot_id: preview ? current.snapshot_id : inventory().snapshot_id, expected_revision: slot.current_revision });
        }, slot.locked || !ready() || !planForRole(draft) || (preview && draft.snapshot_id !== inventory()?.snapshot_id), preview ? "primary" : "secondary"),
        button("web.library_import_game", () => mutate({ action: "import_game", role_id: roleId, slot_id: slot.id, source_snapshot_id: inventory().snapshot_id, expected_revision: slot.current_revision }), slot.locked || !ready() || !selectedRole()?.character),
        button(missingCount ? "web.library_load_partial" : "web.library_load_draft", () => loadDraft(slot.plan), !slot.plan?.character, preview ? "secondary" : "primary"),
      );
      detail.append(actions);
      const management = node("details", "library-management management-section"); management.open = history?.slot_id === slot.id;
      const managementActions = node("div", "page-actions"); managementActions.append(
        button("web.library_history", () => loadHistory(slot)),
        button("web.library_archive", () => mutate({ action: "archive", role_id: roleId, slot_id: slot.id, expected_revision: slot.current_revision }), slot.locked || slots.filter((value) => !value.archived).length <= 1),
      );
      management.append(node("summary", "", t("web.library_management")), rename, managementActions);
      if (preview) {
        detail.append(node("p", "draft-warning", t(draft.snapshot_id === inventory()?.snapshot_id ? "web.library_pending_preview" : "web.optimize_stale_preview")), button("web.library_discard_preview", () => { incomingPreview = null; render(); }));
      }
      if (slot.plan && !slot.plan.character) detail.append(node("p", "meta", t("web.library_software_plan")));
      if (missingCount) detail.append(node("p", "draft-warning", t("web.library_incomplete", missingCount)));
      if (slot.plan) {
        const summary = node("details", "saved-plan-summary");
        summary.append(node("summary", "", t("web.library_plan_summary", slot.plan.placements.length, slot.plan.core ? t("web.core") : t("web.no_core"))));
        for (const placement of [...(slot.plan.core ? [{ equipment: slot.plan.core, core: true }] : []), ...slot.plan.placements]) {
          const item = inventory()?.items?.find((value) => value.uid.slot === placement.equipment.slot && value.uid.serial === placement.equipment.serial);
          const row = node("div", "saved-plan-row"); row.append(equipmentIcon(item, node, "small"), node("span", "saved-plan-name", item?.name || `${placement.equipment.slot}:${placement.equipment.serial}`), node("span", "meta", placement.core ? t("web.core") : t("web.position", placement.row, placement.column))); summary.append(row);
        }
        detail.append(summary);
      }
      if (history?.slot_id === slot.id) {
        const historyView = node("div", "slot-history"); historyView.append(node("h3", "", t("web.library_history")));
        if (!history.revisions.length) historyView.append(node("p", "meta", t("web.library_history_empty")));
        for (const revision of history.revisions) {
          const row = node("div", "slot-history-row"), copy = node("div", "draft-copy");
          const timestamp = new Date(Number(revision.created_at) / 1000);
          copy.append(node("strong", "", t("web.library_revision", revision.id)), node("small", "", Number.isNaN(timestamp.getTime()) ? revision.created_at : timestamp.toLocaleString(document.documentElement.lang || "en")), node("small", "", scoreSummary(revision)));
          row.append(copy, button("web.library_restore", () => mutate({ action: "restore", role_id: roleId, slot_id: slot.id, revision_id: revision.id, expected_revision: slot.current_revision }), slot.locked)); historyView.append(row);
        }
        management.append(historyView);
      }
      detail.append(management);
    }
    body.append(list, detail);
  }

  async function loadSlots() {
    if (busy) return;
    renderRoles(); if (!roleId) { render(); return; }
    busy = true; error = ""; render();
    try { const result = await api(`api/slots/${encodeURIComponent(roleId)}`); slots = result.slots; selected = result.selected_slot_id; }
    catch (failure) { error = failure.message; }
    finally { busy = false; render(); }
  }
  async function loadHistory(slot) {
    busy = true; error = ""; render();
    try { history = { ...await api(`api/slots/${encodeURIComponent(roleId)}/${encodeURIComponent(slot.id)}/history`), slot_id: slot.id }; }
    catch (failure) { error = failure.message; }
    finally { busy = false; render(); }
  }
  async function mutate(request) {
    if (busy) return;
    busy = true; error = ""; render();
    try {
      await api("api/slots", request);
      if (request.action === "save") incomingPreview = null;
      await reloadWorkspace(); history = null;
      const result = await api(`api/slots/${encodeURIComponent(roleId)}`); slots = result.slots; selected = result.selected_slot_id;
    } catch (failure) { error = failure.message; }
    finally { busy = false; render(); }
  }
  return {
    activate() { activated = true; if (!incomingPreview && (requestedRole || currentRole())) { roleId = requestedRole || currentRole(); history = null; } requestedRole = ""; return loadSlots(); },
    selectRole(id) { if (!id) return; requestedRole = id; if (roleId === id) return; roleId = id; history = null; incomingPreview = null; saveRole(id); if (activated) loadSlots(); },
    savePreview(plan) { incomingPreview = structuredClone(plan); roleId = plan.role_id; saveRole(roleId); history = null; return loadSlots(); },
    sync() {
      const previous = roleId; renderRoles();
      if (previous && previous !== roleId) { slots = []; selected = ""; history = null; if (activated) loadSlots(); }
    },
  };
}
