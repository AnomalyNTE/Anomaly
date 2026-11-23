const keyOf = (uid) => uid ? `${uid.slot}:${uid.serial}` : "";
export function createLoadoutSave({ plans, sourceSnapshot, api, t, node, workspace, current, reloadWorkspace, readPreference, savePreference }) {
  let choices = {}, busy = false, saved = false, error = "", message = "";
  try { const saved = JSON.parse(readPreference() || "{}"); if (saved && typeof saved === "object" && !Array.isArray(saved)) choices = saved; }
  catch { error = t("web.batch_save_invalid_preferences"); }
  const element = node("details", "batch-save"); element.append(node("summary", "", t("web.batch_save_title")));
  const fields = node("div", "batch-save-fields"), rows = [];
  for (const plan of plans) {
    const role = workspace()?.roles?.find((role) => plan.role_id ? role.id === plan.role_id : keyOf(role.character) === keyOf(plan.character));
    const label = node("label", "page-field"), select = node("select"); label.append(node("span", "", role?.name || plan.role_id || keyOf(plan.character)), select);
    const slots = (workspace()?.slots || []).filter((slot) => slot.role_id === role?.id && !slot.archived && !slot.locked);
    for (const slot of slots) { const option = node("option", "", slot.name || t("web.library_primary")); option.value = slot.id; select.append(option); }
    select.value = choices[role?.id] || workspace()?.selected_slots?.[role?.id] || slots[0]?.id || "";
    if (!select.value && slots.length) select.value = slots[0].id;
    select.onchange = () => { choices[role.id] = select.value; savePreference(JSON.stringify(choices)); render(); };
    rows.push({ plan, role, select, slots }); fields.append(label);
  }
  const save = node("button", "button primary", t("web.batch_save_action")); save.type = "button";
  const refresh = node("button", "button quiet", t("web.batch_save_refresh")); refresh.type = "button";
  const notice = node("p", "tool-message"); notice.setAttribute("role", "status"); const actions = node("div", "page-actions"); actions.append(save, refresh); element.append(fields, actions, notice);
  function render() {
    for (const row of rows) row.select.disabled = busy || saved || !row.role || !row.select.options.length;
    save.disabled = busy || saved || !current() || rows.some((row) => !row.role || !row.select.value);
    refresh.disabled = busy;
    notice.textContent = error || message || (rows.some((row) => !row.select.options.length) ? t("web.batch_save_unlock_slot") : ""); notice.hidden = !notice.textContent; notice.classList.toggle("error", Boolean(error));
  }
  save.onclick = async () => {
    if (busy || !current() || save.disabled) return;
    const entries = rows.map(({ plan, role, select, slots }) => {
      const slot = slots.find((slot) => slot.id === select.value);
      return { role_id: role.id, slot_id: slot.id, expected_revision: slot.current_revision,
        plan: { character: plan.character, role_id: role.id, core: plan.core, placements: plan.placements } };
    });
    busy = true; error = ""; message = ""; render();
    try {
      const result = await api("api/slots/batch", { source_snapshot_id: sourceSnapshot, entries });
      await reloadWorkspace(); saved = true; message = t("web.batch_save_complete", result.saved);
    } catch (failure) { error = failure.message; }
    finally { busy = false; render(); }
  };
  refresh.onclick = async () => {
    if (busy) return; busy = true; error = ""; render();
    try {
      await reloadWorkspace();
      const replacement = createLoadoutSave({ plans, sourceSnapshot, api, t, node, workspace, current, reloadWorkspace, readPreference, savePreference });
      replacement.open = element.open; element.replaceWith(replacement);
    }
    catch (failure) { error = failure.message; }
    finally { busy = false; render(); }
  };
  render(); return element;
}
