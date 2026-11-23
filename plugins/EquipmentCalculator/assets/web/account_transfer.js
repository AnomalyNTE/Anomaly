export function createAccountTransfer({ mount, api, t, node, workspace, enabled, busy, setBusy, canStart, reloadWorkspace }) {
  let working = false, error = "", message = "";
  const section = node("section", "account-transfer");
  section.append(node("h3", "", t("web.transfer_title")), node("p", "meta", t("web.transfer_hint")));
  const actions = node("div", "page-actions"), exportButton = node("button", "button secondary", t("web.transfer_export")); exportButton.type = "button";
  actions.append(exportButton);
  const form = node("form", "transfer-import"), fileLabel = node("label", "page-field"), file = node("input");
  file.type = "file"; file.accept = ".zip,application/zip"; file.required = true;
  fileLabel.append(node("span", "", t("web.transfer_file")), file);
  const nameLabel = node("label", "page-field"), name = node("input"); name.required = true; name.maxLength = 128;
  nameLabel.append(node("span", "", t("web.transfer_name")), name);
  file.onchange = () => { if (file.files[0] && !name.value) name.value = file.files[0].name.replace(/\.zip$/i, ""); };
  const importButton = node("button", "button primary", t("web.transfer_import")); importButton.type = "submit";
  form.append(fileLabel, nameLabel, importButton);
  const notice = node("p", "transfer-message"); notice.setAttribute("role", "status");
  section.append(actions, form, notice); mount.append(section);

  function render() {
    section.hidden = !enabled();
    const disabled = busy() || !workspace()?.account;
    for (const control of [exportButton, importButton, file, name]) control.disabled = disabled;
    notice.textContent = error || message; notice.hidden = !error && !message;
    notice.classList.toggle("page-error", Boolean(error));
  }
  function progress(completed, total) { message = t("web.transfer_progress", Math.ceil(completed / 1024), Math.ceil(total / 1024)); render(); }
  async function perform(action) {
    if (working || busy() || !canStart()) return;
    working = true; error = ""; message = ""; setBusy(true); render();
    try { await action(); }
    catch (failure) { error = failure.message; }
    finally { working = false; setBusy(false); render(); }
  }
  exportButton.onclick = () => perform(async () => {
    const account = workspace().account;
    const transfer = await api("api/accounts/export", { id: account.id });
    let offset = 0; const chunks = [];
    while (offset < transfer.total_bytes) {
      const chunk = await api(`api/accounts/export/${encodeURIComponent(transfer.transfer_id)}/${offset}`);
      let data;
      try { data = Uint8Array.from(atob(chunk.data_base64), (character) => character.charCodeAt(0)); }
      catch { throw new Error(t("web.transfer_invalid_chunk")); }
      if (chunk.offset !== offset || !data.length || (chunk.next_offset != null && chunk.next_offset !== offset + data.length)) throw new Error(t("web.transfer_invalid_chunk"));
      chunks.push(data); offset += data.length; progress(offset, transfer.total_bytes);
    }
    if (offset !== transfer.total_bytes) throw new Error(t("web.transfer_invalid_chunk"));
    const url = URL.createObjectURL(new Blob(chunks, { type: "application/zip" }));
    const download = node("a"); download.href = url; download.download = `${account.name || account.id}.zip`; download.hidden = true;
    document.body.append(download); download.click(); download.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
    message = t("web.transfer_download_ready");
  });
  form.onsubmit = (event) => {
    event.preventDefault(); if (!form.reportValidity()) return;
    const archive = file.files[0]; if (!archive) return;
    perform(async () => {
      const transfer = await api("api/accounts/import/start", { name: name.value, total_bytes: archive.size });
      let offset = 0;
      while (offset < archive.size) {
        const bytes = new Uint8Array(await archive.slice(offset, offset + 32768).arrayBuffer());
        let binary = ""; for (const byte of bytes) binary += String.fromCharCode(byte);
        const chunk = await api("api/accounts/import/chunk", { transfer_id: transfer.transfer_id, offset, data_base64: btoa(binary) });
        if (chunk.next_offset !== offset + bytes.length) throw new Error(t("web.transfer_invalid_chunk"));
        offset = chunk.next_offset; progress(offset, archive.size);
      }
      await api("api/accounts/import/finish", { transfer_id: transfer.transfer_id });
      await reloadWorkspace(); file.value = ""; name.value = ""; message = t("web.transfer_imported");
    });
  };
  return { render };
}
