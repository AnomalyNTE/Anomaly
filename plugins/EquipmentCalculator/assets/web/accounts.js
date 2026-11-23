import { createAccountTransfer } from "./account_transfer.js";

export function createAccountsPanel({ mount, api, t, node, workspace, features, reloadWorkspace, selectedAccountChanged }) {
  let busy = false, error = "", confirmDelete = "";
  const edits = new Map();
  const heading = node("div", "page-heading");
  heading.append(node("div")); heading.firstChild.append(node("h2", "", t("web.accounts_title")), node("p", "meta", t("web.accounts_hint")));
  const refresh = node("button", "button quiet", t("web.refresh")); refresh.type = "button"; refresh.onclick = () => reload(); heading.append(refresh);
  const discard = node("button", "button quiet", t("web.accounts_discard_names")); discard.type = "button"; discard.onclick = () => { edits.clear(); error = ""; render(); }; heading.append(discard);
  const status = node("p", "account-identity"), notice = node("p", "page-error"), list = node("div", "account-list"); notice.setAttribute("role", "alert");
  const create = node("form", "account-create"), createLabel = node("label", "page-field"), newName = node("input"), submit = node("button", "button primary", t("web.accounts_create"));
  newName.required = true; newName.maxLength = 128; createLabel.append(node("span", "", t("web.accounts_new_name")), newName); submit.type = "submit";
  create.append(createLabel, submit); create.onsubmit = (event) => { event.preventDefault(); if (create.reportValidity()) mutate({ action: "create", name: newName.value }); };
  mount.append(heading, status, notice, list, create);
  const transfer = createAccountTransfer({ mount, api, t, node, workspace,
    enabled: () => features().account_transfer === true, busy: () => busy,
    setBusy: (value) => { busy = value; render(); }, reloadWorkspace,
    canStart: () => {
      if (!edits.size) return true;
      error = t("web.accounts_unsaved_names"); notice.textContent = error; notice.hidden = false; return false;
    },
  });
  function render() {
    const data = workspace();
    status.textContent = t(!data?.live_identity_available ? "web.identity_waiting" : data.live_account_matches ? "web.account_matches" : "web.account_mismatch");
    status.classList.toggle("unmatched", !data?.live_account_matches);
    refresh.disabled = busy; submit.disabled = busy; discard.hidden = !edits.size; discard.disabled = busy;
    notice.textContent = error; notice.hidden = !error;
    list.replaceChildren();
    for (const account of data?.accounts || []) {
      const active = account.id === data.account.id;
      const card = node("div", `account-card${active ? " active" : ""}`), form = node("form", "account-name-form"), label = node("label", "page-field"), input = node("input");
      label.append(node("span", "", t(active ? "web.accounts_current" : "web.accounts_name")));
      input.value = edits.get(account.id) ?? account.name; input.required = true; input.maxLength = 128; input.disabled = busy;
      input.oninput = () => { edits.set(account.id, input.value); discard.hidden = false; };
      label.append(input);
      const rename = node("button", "button quiet", t("web.accounts_rename")); rename.type = "submit"; rename.disabled = busy;
      form.append(label, rename); form.onsubmit = (event) => { event.preventDefault(); if (form.reportValidity()) mutate({ action: "rename", id: account.id, name: input.value }); };
      const actions = node("div", "page-actions");
      const select = node("button", "button secondary", t(active ? "web.accounts_selected" : "web.accounts_select")); select.type = "button"; select.disabled = busy || active;
      select.onclick = () => {
        if (edits.size) { error = t("web.accounts_unsaved_names"); notice.textContent = error; notice.hidden = false; return; }
        mutate({ action: "select", id: account.id }, true);
      };
      const remove = node("button", "button quiet danger", t("web.accounts_delete")); remove.type = "button"; remove.disabled = busy || active || (data.accounts || []).length <= 1;
      if (active) remove.title = t("web.accounts_switch_before_delete");
      remove.onclick = () => { confirmDelete = account.id; render(); };
      const bind = node("button", "button quiet", t("web.accounts_bind_live")); bind.type = "button"; bind.disabled = busy || !data.live_identity_available || (active && data.live_account_matches);
      bind.onclick = () => mutate({ action: "bind_live", id: account.id });
      actions.append(select, bind, remove); card.append(form, actions);
      if (confirmDelete === account.id) {
        const confirmation = node("div", "account-delete-confirmation"); confirmation.append(node("p", "", t("web.accounts_delete_confirmation", account.name)));
        const yes = node("button", "button quiet danger", t("web.accounts_delete_confirm")); yes.type = "button"; yes.disabled = busy;
        yes.onclick = () => mutate({ action: "delete", id: account.id }, active);
        const no = node("button", "button quiet", t("web.accounts_keep")); no.type = "button"; no.onclick = () => { confirmDelete = ""; render(); };
        confirmation.append(yes, no); card.append(confirmation);
      }
      list.append(card);
    }
    transfer.render();
  }
  async function reload() {
    if (busy) return;
    busy = true; error = ""; render();
    try { await reloadWorkspace(); } catch (failure) { error = failure.message; }
    finally { busy = false; render(); }
  }
  async function mutate(body, changing = false) {
    if (busy) return;
    busy = true; error = ""; render();
    try {
      await api("api/accounts", body);
      edits.delete(body.id); confirmDelete = "";
      if (changing) { selectedAccountChanged(); return; }
      await reloadWorkspace(); newName.value = "";
    } catch (failure) { error = failure.message; }
    finally { busy = false; render(); }
  }
  return { activate: render, sync() { if (!edits.size && !mount.contains(document.activeElement)) render(); } };
}
