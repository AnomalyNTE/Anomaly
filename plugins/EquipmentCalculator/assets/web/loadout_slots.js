export function createLoadoutSlots({ mount, api, t, node, workspace, inventory, role, draft, current,
  loadPreview, reloadWorkspace, manage }) {
  let busy = false, error = "", message = "", signature = "";
  const body = node("div", "home-slots"), status = node("p", "slot-message"); status.setAttribute("role", "status");
  const footer = node("div", "home-slots-footer"), open = node("button", "button quiet", t("web.manage_schemes"));
  open.type = "button"; open.onclick = () => manage(role()?.id); footer.append(open); mount.append(body, status, footer);
  const number = (value) => new Intl.NumberFormat(document.documentElement.lang || "en", { maximumFractionDigits: 1 }).format(value);
  const slotName = (slot) => slot.name || t("web.library_primary");
  async function perform(action) {
    if (busy) return;
    busy = true; error = ""; message = ""; render();
    try { await action(); }
    catch (failure) { error = failure.message; }
    finally { busy = false; signature = ""; render(); }
  }
  function render() {
    const selected = role(), slots = (workspace()?.slots || []).filter((slot) => slot.role_id === selected?.id && !slot.archived);
    const preview = draft();
    status.textContent = error || message; status.hidden = !status.textContent; status.classList.toggle("error", Boolean(error));
    open.disabled = !selected || busy;
    const next = JSON.stringify([selected?.id, slots, preview, busy, current()]);
    if (signature === next) return; signature = next;
    body.replaceChildren();
    if (!selected) { body.append(node("p", "empty compact", t("web.choose_character"))); return; }
    for (const slot of slots) {
      const card = node("section", `home-slot${slot.plan ? " filled" : " empty-slot"}`);
      const pick = node("button", "slot-preview"); pick.type = "button"; pick.disabled = busy || !slot.plan;
      const name = slotName(slot); pick.append(node("strong", "", name));
      pick.append(node("span", "meta", slot.plan ? slot.score == null ? t("web.scheme_piece_count", slot.plan.placements.length) : `${number(slot.score)} · ${slot.grade ?? "—"}` : t("web.empty_scheme")));
      if (slot.locked) pick.append(node("span", "tag locked", t("web.library_locked")));
      pick.setAttribute("aria-label", t("web.preview_named_scheme", name));
      pick.onclick = () => perform(async () => {
        const frozen = await api(`api/slots/${encodeURIComponent(selected.id)}/${encodeURIComponent(slot.id)}`);
        if (role()?.id !== selected.id) return;
        loadPreview(frozen.slot.plan, { kind: "saved", role_id: selected.id, name, score: frozen.slot.score, grade: frozen.slot.grade, items: frozen.items, allowed_cells: frozen.role?.allowed_cells });
      });
      const save = node("button", "button quiet", t(preview ? "web.library_save_preview" : "web.library_import_game")); save.type = "button";
      save.setAttribute("aria-label", t(preview ? "web.save_preview_named" : "web.save_current_named", name));
      save.disabled = busy || slot.locked || !current();
      save.onclick = () => perform(async () => {
        const request = { action: preview ? "save" : "import_game", role_id: selected.id, slot_id: slot.id,
          source_snapshot_id: inventory().snapshot_id, expected_revision: slot.current_revision };
        if (preview) request.plan = preview;
        await api("api/slots", request); await reloadWorkspace(); message = t("web.scheme_saved_named", name);
      });
      card.append(pick, save); body.append(card);
    }
    if (slots.length < 3) {
      const create = node("details", "home-slot-add"), form = node("form", "home-slot-create"), name = node("input"); name.required = true; name.maxLength = 128; name.placeholder = t("web.library_new_name"); name.setAttribute("aria-label", t("web.library_new_name")); name.disabled = busy;
      const add = node("button", "button secondary", t("web.library_create")); add.type = "submit"; add.disabled = busy;
      form.onsubmit = (event) => { event.preventDefault(); if (!form.reportValidity()) return; const value = name.value; perform(async () => { await api("api/slots", { action: "create", role_id: selected.id, name: value }); await reloadWorkspace(); }); };
      form.append(name, add); create.append(node("summary", "", t("web.library_create")), form); body.append(create);
    }
  }
  return { sync: render };
}
