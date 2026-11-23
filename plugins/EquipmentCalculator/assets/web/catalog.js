const domains = [
  { id: "inventory", sections: ["inventory", "items", "player_pack", "material_recipes"] },
  { id: "equipment", sections: ["equipment", "forks", "fork_templates", "equipment_suits", "attributes"] },
  { id: "characters", sections: ["characters", "character_skills", "role_skills"] },
  { id: "growth", sections: ["character_upgrade", "character_breakthrough", "fork_upgrade", "fork_breakthrough", "fork_refinement"] },
  { id: "modifiers", sections: ["player_modify", "fork_modify", "equipment_modify", "likeability_modify"] },
];
export function createCatalogPanel({ mount, api, t, node, readPreference, savePreference }) {
  let preferences = { section: "characters", offset: 0 }, loaded = false, posting = false, reading = false, error = "", job = { status: "idle" }, page = null, timer;
  let renderedPage, renderedStale, renderedActive;
  const heading = node("div", "page-heading"); heading.append(node("div")); heading.firstChild.append(node("h2", "", t("web.catalog_title")), node("p", "meta", t("web.catalog_hint")));
  const phase = node("span", "badge"); heading.append(phase);
  const toolbar = node("div", "catalog-toolbar"), domainLabel = node("label", "page-field"), domain = node("select"), sectionLabel = node("label", "page-field"), section = node("select");
  for (const value of domains) { const option = node("option", "", t(`web.catalog_domain_${value.id}`)); option.value = value.id; domain.append(option); }
  domainLabel.append(node("span", "", t("web.catalog_domain")), domain); sectionLabel.append(node("span", "", t("web.catalog_section")), section);
  const read = node("button", "button primary", t("web.catalog_read")); read.type = "button"; read.onclick = () => readPage(preferences.offset);
  const refreshStatus = node("button", "button quiet", t("web.catalog_check_status")); refreshStatus.type = "button"; refreshStatus.onclick = () => poll();
  toolbar.append(domainLabel, sectionLabel, read, refreshStatus);
  const notice = node("p", "tool-message"); notice.setAttribute("role", "status");
  const output = node("div", "catalog-results"), paging = node("div", "catalog-paging"), first = node("button", "button quiet", t("web.catalog_first")), previous = node("button", "button quiet", t("web.previous_page")), next = node("button", "button quiet", t("web.next_page")), range = node("span", "meta");
  for (const button of [first, previous, next]) button.type = "button";
  first.onclick = () => readPage(0); previous.onclick = () => readPage(Math.max(0, page.offset - 32)); next.onclick = () => { if (page?.next_offset != null) readPage(page.next_offset); };
  paging.append(first, previous, range, next); mount.append(heading, toolbar, notice, output, paging);
  const active = () => ["queued", "running"].includes(job.status);
  function save() { savePreference(JSON.stringify(preferences)); }
  function populateSections() {
    const selected = domains.find((value) => value.id === domain.value) || domains[2];
    section.replaceChildren(...selected.sections.map((id) => { const option = node("option", "", t(`web.catalog_section_${id}`)); option.value = id; return option; })); section.value = preferences.section;
  }
  domain.onchange = () => { preferences.section = domains.find((value) => value.id === domain.value).sections[0]; preferences.offset = 0; page = null; populateSections(); save(); render(); };
  section.onchange = () => { preferences.section = section.value; preferences.offset = 0; page = null; save(); render(); };
  function initialize() {
    try {
      const saved = JSON.parse(readPreference() || "{}");
      if (!saved || !domains.some((domain) => domain.sections.includes(saved.section ?? preferences.section)) || (Object.hasOwn(saved, "offset") && (!Number.isSafeInteger(saved.offset) || saved.offset < 0))) throw new Error();
      preferences = { ...preferences, ...saved };
    } catch { error = t("web.catalog_invalid_preferences"); }
    domain.value = domains.find((value) => value.sections.includes(preferences.section)).id; populateSections(); loaded = true;
  }
  async function readPage(offset) {
    if (posting || active()) return;
    posting = true; error = ""; render();
    try {
      job = await api("api/catalog/read", { section: preferences.section, offset, limit: 32 });
      preferences.offset = offset; save(); await poll();
    } catch (failure) { error = failure.message; }
    finally { posting = false; render(); }
  }
  async function poll() {
    if (reading || !job.id) return;
    reading = true; clearTimeout(timer); render();
    try {
      job = await api(`api/catalog/jobs/${encodeURIComponent(job.id)}`); error = "";
      if (job.status === "completed") page = job.result;
      if (active()) timer = setTimeout(poll, 500);
    } catch (failure) { error = failure.message; }
    finally { reading = false; render(); }
  }
  function renderRecord(record, index) {
    const details = node("details", "catalog-record");
    const label = record.name || record.display_name || record.id || record.item_id || record.row_name || t("web.catalog_record", page.offset + index + 1);
    details.append(node("summary", "", label));
    const table = node("table", "data-table"), body = node("tbody");
    for (const [key, value] of Object.entries(record)) {
      const row = node("tr"), heading = node("th", "mono", key), cell = node("td"); heading.scope = "row";
      if (value && typeof value === "object") {
        const nested = node("details", "catalog-nested"); nested.append(node("summary", "", t("web.catalog_expand", Array.isArray(value) ? value.length : Object.keys(value).length)), node("pre", "mono", JSON.stringify(value, null, 2))); cell.append(nested);
      } else cell.append(node("span", value == null ? "meta" : "", value == null ? "—" : value));
      row.append(heading, cell); body.append(row);
    }
    table.append(body); details.append(table); return details;
  }
  function render() {
    const disabled = !loaded || posting || active();
    domain.disabled = disabled; section.disabled = disabled; read.disabled = disabled;
    refreshStatus.disabled = reading || !job.id; refreshStatus.hidden = !active() || !error;
    phase.textContent = t(`web.optimize_status_${job.status || "idle"}`);
    notice.textContent = error || job.message || ""; notice.hidden = !notice.textContent; notice.classList.toggle("error", Boolean(error) || job.status === "failed");
    first.disabled = posting || active(); previous.disabled = disabled || !page || page.offset === 0; next.disabled = disabled || !page || page.next_offset == null;
    range.textContent = page ? t("web.catalog_range", page.offset + (page.rows.length ? 1 : 0), page.offset + page.rows.length, page.total) : "";
    if (page === renderedPage && job.stale === renderedStale && active() === renderedActive) return;
    renderedPage = page; renderedStale = job.stale; renderedActive = active();
    output.replaceChildren();
    if (!page) { output.append(node("div", "empty", t(active() ? "web.catalog_loading" : "web.catalog_waiting"))); return; }
    output.append(node("h3", "", t(`web.catalog_section_${page.section || preferences.section}`)));
    if (job.stale) output.append(node("p", "draft-warning", t("web.catalog_stale")));
    const coverage = node("p", "meta", t(page.source_kind === "live_data_table" ? "web.catalog_source_table" : "web.catalog_source_inventory"));
    coverage.append(document.createTextNode(` · ${t(page.source_coverage === "loaded_table" ? "web.catalog_coverage_table" : "web.catalog_coverage_unknown")}`)); output.append(coverage);
    if (page.collection_complete !== true) output.append(node("p", "draft-warning", t("web.catalog_partial")));
    if (!page.rows.length) output.append(node("p", "empty", t("web.catalog_empty")));
    for (const [index, record] of page.rows.entries()) output.append(renderRecord(record, index));
  }
  return { activate() { if (!loaded) initialize(); render(); }, sync() { if (!loaded) initialize(); render(); } };
}
