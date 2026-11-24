import { observedEquipmentCatalog, shapeThumbnail } from "./equipment_catalog.js";
import { renderSearchSummary } from "./optimization_results.js";
import { arrangeBoard } from "./board_layout.js";

const defaults = { role_id: "", counts: {}, set_effect_mode: "none", suit_id: "", extra_shape_area: "", layout_variants: false, max_solutions: 100, max_nodes: 2000000, time_limit_ms: 10000 };
export function createBlueprintPanel({ mount, t, node, inventory, workspace, jobController, readPreference, savePreference }) {
  let preferences = structuredClone(defaults), resultPage = 1, resultJob, loaded = false, signature = "", error = "", renderedOutputKey = "";
  const heading = node("div", "page-heading"); heading.append(node("div")); heading.firstChild.append(node("h2", "", t("web.blueprint_title")), node("p", "meta", t("web.blueprint_hint")));
  const phase = node("span", "badge"); heading.append(phase);
  const form = node("form", "tool-form"), options = node("div", "tool-options"), fields = {};
  for (const [field, key] of [["role_id", "web.blueprint_board"], ["suit_id", "web.filter_suit"], ["set_effect_mode", "web.optimize_set_effect"], ["extra_shape_area", "web.blueprint_extra_area"]]) {
    const label = node("label", "page-field"), select = node("select"); select.onchange = () => { preferences[field] = select.value; persist(); };
    label.append(node("span", "", t(key)), select); fields[field] = select; options.append(label);
  }
  for (const [field, key, min, max, scale = 1] of [["max_solutions", "web.blueprint_max_solutions", 1, 2000], ["max_nodes", "web.optimize_node_limit", 1, 100000000], ["time_limit_ms", "web.optimize_time_limit_seconds", 0.001, 120, 1000]]) {
    const label = node("label", "page-field"), input = node("input"); input.type = "number"; input.min = min; input.max = max; input.step = scale === 1000 ? "0.001" : "1"; input.required = true;
    input.oninput = () => { if (input.checkValidity()) { preferences[field] = Math.round(input.valueAsNumber * scale); persist(); } }; fields[field] = input;
    label.append(node("span", "", t(key)), input); options.append(label);
  }
  const variants = node("label", "optimizer-check"), variantsInput = node("input"); variantsInput.type = "checkbox";
  variantsInput.onchange = () => { preferences.layout_variants = variantsInput.checked; persist(); }; variants.append(variantsInput, node("span", "", t("web.blueprint_variants"))); fields.layout_variants = variantsInput;
  const shapes = node("div", "blueprint-shapes"), shapesLabel = node("h3", "", t("web.blueprint_shapes"));
  const actions = node("div", "page-actions"), run = node("button", "button primary", t("web.blueprint_run")), cancel = node("button", "button quiet", t("web.optimize_cancel"));
  run.type = "submit"; cancel.type = "button"; cancel.onclick = () => jobController.cancel(); actions.append(run, cancel);
  form.append(options, variants, shapesLabel, node("p", "meta", t("web.blueprint_count_hint")), shapes, actions);
  const notice = node("p", "tool-message"); notice.setAttribute("role", "status"); const output = node("div", "blueprint-results");
  mount.append(heading, form, notice, output);
  function persist() { error = ""; savePreference(JSON.stringify(preferences)); render(); }
  function initialize() {
    try {
      const saved = JSON.parse(readPreference() || "{}");
      if (!saved || typeof saved !== "object" || Array.isArray(saved) || (Object.hasOwn(saved, "counts") && (!saved.counts || typeof saved.counts !== "object" || Array.isArray(saved.counts)))) throw new Error();
      preferences = { ...preferences, ...saved };
    } catch { error = t("web.blueprint_invalid_preferences"); }
    for (const field of ["max_solutions", "max_nodes", "time_limit_ms"]) fields[field].value = field === "time_limit_ms" ? preferences[field] / 1000 : preferences[field];
    variantsInput.checked = preferences.layout_variants === true; loaded = true;
  }
  function selectOptions(field, entries) {
    fields[field].replaceChildren(...entries.map(([value, label]) => { const option = node("option", "", label); option.value = value; return option; })); fields[field].value = preferences[field];
  }
  function catalog() {
    const data = observedEquipmentCatalog(inventory(), t), roles = (workspace()?.roles || []).filter((role) => !role.archived && role.allowed_cells?.length);
    const next = JSON.stringify([data.shapes, data.suits, roles.map((role) => [role.id, role.name, role.allowed_cells])]); if (signature === next) return; signature = next;
    selectOptions("role_id", [["", t("web.blueprint_choose_board")], ...roles.map((role) => [role.id, role.name])]);
    selectOptions("suit_id", [["", t("web.optimize_any_suit")], ...data.suits.map((suit) => [suit.id, suit.name])]);
    selectOptions("set_effect_mode", [["none", t("web.set_none")], ["two_piece", t("web.set_two_piece")], ["four_piece", t("web.set_four_piece")]]);
    selectOptions("extra_shape_area", [["", t("web.blueprint_no_extra")], ...[...new Set(data.shapes.map((shape) => shape.cells.length))].sort().map((area) => [String(area), t("web.blueprint_area", area)])]);
    shapes.replaceChildren();
    for (const shape of data.shapes) {
      const label = node("label", "blueprint-shape"), input = node("input"); input.type = "number"; input.min = "0"; input.max = "25"; input.step = "1"; input.required = true; input.value = preferences.counts[shape.shape_id] ?? 25;
      input.setAttribute("aria-label", t("web.blueprint_count_for", shape.shape_id)); input.oninput = () => { if (input.checkValidity()) { preferences.counts[shape.shape_id] = input.valueAsNumber; persist(); } };
      label.append(shapeThumbnail(shape, node), node("span", "mono", shape.shape_id), input); shapes.append(label);
    }
    if (!data.shapes.length) shapes.append(node("p", "meta", t("web.blueprint_no_shapes")));
  }
  function renderBoard(blueprint, result) {
    if (!Array.isArray(result.allowed_cells) || !Array.isArray(result.shapes)) return null;
    const occupied = new Map();
    for (const [index, placement] of blueprint.placements.entries()) {
      const shape = result.shapes.find((value) => value.shape_id === placement.shape_id); if (!shape) return null;
      for (const [row, column] of shape.cells) occupied.set(`${placement.row + row}:${placement.column + column}`, { index, placement });
    }
    const board = node("div", "blueprint-board"); board.setAttribute("role", "img"); board.setAttribute("aria-label", t("web.blueprint_board_preview"));
    for (const { row, column, gridRow, gridColumn } of arrangeBoard(board, result.allowed_cells)) {
      const key = `${row}:${column}`, piece = occupied.get(key), cell = node("span", "blueprint-cell allowed");
      cell.style.gridRow = String(gridRow); cell.style.gridColumn = String(gridColumn);
      if (piece) { cell.dataset.piece = String(piece.index % 6); cell.title = t("web.blueprint_shape_at", piece.placement.shape_id, piece.placement.row, piece.placement.column); if (piece.placement.row === row && piece.placement.column === column) cell.textContent = piece.placement.shape_id; }
      board.append(cell);
    }
    return board;
  }
  function renderResults() {
    const { job } = jobController.state(), result = job.result;
    if (job.id !== resultJob) { resultJob = job.id; resultPage = 1; }
    const key = `${job.id || ""}|${job.status}|${resultPage}`; if (key === renderedOutputKey) return; renderedOutputKey = key;
    output.replaceChildren();
    if (job.status !== "completed" || !Array.isArray(result?.blueprints)) return;
    output.append(renderSearchSummary({ result, t, node }));
    output.append(node("p", "meta", t("web.blueprint_result_count", result.blueprints.length)));
    if (!result.blueprints.length) output.append(node("p", "draft-warning", t("web.optimize_no_solution")));
    const pages = Math.max(1, Math.ceil(result.blueprints.length / 20)), page = Math.min(pages, resultPage);
    const paging = node("div", "result-paging"), previous = node("button", "button quiet", t("web.previous_page")), next = node("button", "button quiet", t("web.next_page"));
    previous.type = "button"; next.type = "button"; previous.disabled = page === 1; next.disabled = page === pages;
    const go = (value) => { resultPage = value; renderResults(); };
    previous.onclick = () => go(page - 1); next.onclick = () => go(page + 1); paging.append(previous, node("span", "meta", t("web.inventory_page", page, pages)), next); output.append(paging);
    for (const [relative, blueprint] of result.blueprints.slice((page - 1) * 20, page * 20).entries()) {
      const index = (page - 1) * 20 + relative;
      const details = node("details", "blueprint-result"); details.open = relative === 0;
      details.append(node("summary", "", t("web.blueprint_result", index + 1, Object.entries(blueprint.shape_counts).map(([id, count]) => `${id} × ${count}`).join(" · "))));
      const content = node("div", "blueprint-result-content"), board = renderBoard(blueprint, result); if (board) content.append(board);
      const list = node("div", "blueprint-placement-list");
      for (const placement of blueprint.placements) { const row = node("div", "saved-plan-row"); row.append(node("span", "mono", placement.shape_id), node("span", "meta", t("web.position", placement.row, placement.column))); list.append(row); }
      content.append(list); details.append(content); output.append(details);
    }
  }
  function render() {
    const { job, active, posting, cancelling, error: jobError } = jobController.state(), disabled = !loaded || active || posting;
    phase.textContent = t(`web.optimize_status_${job.status || "idle"}`);
    for (const control of form.querySelectorAll("input,select")) control.disabled = disabled;
    fields.suit_id.disabled = disabled || preferences.set_effect_mode === "none";
    run.disabled = disabled || !workspace()?.account || !preferences.role_id || !observedEquipmentCatalog(inventory(), t).shapes.length;
    cancel.disabled = !active || posting || cancelling;
    cancel.textContent = t(cancelling ? "web.search_progress_cancelling" : "web.optimize_cancel");
    notice.textContent = error || jobError || (job.kind === "blueprints" ? job.message : "") || ""; notice.hidden = !notice.textContent;
    notice.classList.toggle("error", Boolean(error || jobError) || job.status === "failed");
    renderResults();
  }
  form.onsubmit = async (event) => {
    event.preventDefault(); if (!form.reportValidity()) return;
    const role = workspace()?.roles?.find((role) => role.id === preferences.role_id), data = observedEquipmentCatalog(inventory(), t);
    if (!role?.allowed_cells?.length) { error = t("web.blueprint_choose_board"); render(); return; }
    const suit = data.suits.find((suit) => suit.id === preferences.suit_id);
    if (preferences.set_effect_mode !== "none" && !suit) { error = t("web.blueprint_require_suit"); render(); return; }
    const selected = data.shapes.filter((shape) => (preferences.counts[shape.shape_id] ?? 25) > 0).map((shape) => ({ ...shape, count: preferences.counts[shape.shape_id] ?? 25 }));
    if (!selected.length) { error = t("web.blueprint_select_shapes"); render(); return; }
    error = "";
    await jobController.run("api/blueprints", { allowed_cells: role.allowed_cells, shapes: selected, set_effect_mode: preferences.set_effect_mode,
      ...(suit && preferences.set_effect_mode !== "none" ? { required_shapes: suit.required_shapes } : {}),
      ...(preferences.extra_shape_area ? { extra_shape_area: Number(preferences.extra_shape_area) } : {}),
      layout_variants: preferences.layout_variants, max_solutions: preferences.max_solutions, max_nodes: preferences.max_nodes, time_limit_ms: preferences.time_limit_ms });
  };
  jobController.subscribe(render);
  return { activate() { if (!loaded) initialize(); catalog(); render(); }, sync() { if (!workspace()?.account) return; if (!loaded) initialize(); catalog(); render(); } };
}
