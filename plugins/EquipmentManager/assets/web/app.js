import { equipmentIcon, updateEquipmentIcon, shapeGlyph, characterAvatar, updateCharacterAvatar, refreshCharacterAvatars } from "./asset_images.js";
import { createAvatarImages } from "./avatar_images.js";
import { fallbackMessages } from "./messages.js";
import { applyPalette } from "./appearance.js";
import { createOptimizer } from "./optimizer.js";
import { createApi } from "./client.js";
import { createWarehouseFilters } from "./warehouse.js";
import { createWorkspacePages } from "./workspace_pages.js";
import { createOptimizationJob } from "./optimization_job.js";
import { createSearchProgress } from "./search_progress.js";
import { arrangeBoard } from "./board_layout.js";
import { createLoadoutSlots } from "./loadout_slots.js";
import { loadoutMatchesInventory, sameLoadout } from "./loadout_state.js";
import { propertyLabel, qualityLabel, suitLabel } from "./equipment_display.js";
import { createCharacterRail } from "./character_rail.js";
import { setRecommendedWeightsCatalog, getRecommendedWeightsCatalog } from "./weight_preferences.js";

const $ = (id) => document.getElementById(id);
const token = new URLSearchParams(location.hash.slice(1)).get("token") || "";
const model = {
  messages: { ...fallbackMessages }, state: null, workspace: null, features: {}, connected: false, bootstrapped: false, requestBusy: false, changingAccount: false,
  selected: "", checked: new Set(), view: "current", preview: null, inspectPreview: false, hoverAnchor: null, placementMode: false, operationId: null, pendingAction: "",
  settings: { filters: { search: "", kind: "all", state: "all", owner: "all", loadout_view: "current", workbench_view: "optimize", color_theme: "naiwa", inventory_page: "1" }, selected_character: null, draft: null },
  saveTimer: null, savePending: false, saving: false,
  settingsEdit: 0, submittedSettingsEdit: 0, settingsStatus: "",
  notice: null,
  syncPosting: false, syncRequest: null,
};
const api = createApi({ token, t, account: () => model.workspace?.account });
const colors = ["#69e5c3", "#85b9fa", "#dfb080", "#ba9df0", "#e6a6bf", "#a8d581", "#78cbd5", "#e2d078"];
let optimizer, warehouse, pages, optimizationJob, loadoutSlots, avatarImages, characterRail;
let workspaceRead = 0;
let refreshPromise = null, refreshSequence = 0;
let pendingLoadout = null;
let syncPromise = null;
let recommendedWeightsTimer = null, recommendedWeightsPolls = 0;
const inventoryRows = new Map(), characterRows = new Map(), selectedAvatars = new Map();
const loadoutEquipmentRows = new Map();
let boardVisual = "", boardLayout = { cells: new Map() }, coreContent = null;
let boardHoverKey = "", boardPositionChosen = false, boardHintContent = null;
let activityVisual = "";
let dynamicInventoryPageSize = 16, dynamicInventoryCols = 0, inventoryWheelAt = 0, inventoryResizeObserver = null;

function t(key, ...values) {
  return (model.messages[key] ?? key).replace(/\{([0-7])\}/g, (_, index) => String(values[Number(index)] ?? ""));
}

function node(tag, className, text) {
  const element = document.createElement(tag);
  if (className) element.className = className;
  if (text !== undefined) element.textContent = String(text);
  return element;
}

function reconcileChildren(container, children) {
  const keep = new Set(children);
  for (const child of [...container.children]) if (!keep.has(child)) child.remove();
  children.forEach((child, index) => { if (container.children[index] !== child) container.insertBefore(child, container.children[index] || null); });
}

function selectedAvatar(id, character, variant) {
  let avatar = selectedAvatars.get(id);
  if (!character) { if (avatar) { avatar.remove(); selectedAvatars.delete(id); } return; }
  if (!avatar) { avatar = characterAvatar(character, node, variant); selectedAvatars.set(id, avatar); $(id).append(avatar); }
  else updateCharacterAvatar(avatar, character);
}

function translateDocument() {
  document.querySelectorAll("[data-i18n]").forEach((element) => { element.textContent = t(element.dataset.i18n); });
  document.querySelectorAll("[data-i18n-aria]").forEach((element) => { element.setAttribute("aria-label", t(element.dataset.i18nAria)); });
  document.querySelectorAll("[data-i18n-placeholder]").forEach((element) => { element.setAttribute("placeholder", t(element.dataset.i18nPlaceholder)); });
  document.title = t("web.brand");
}

function uidKey(uid) { return uid == null ? "" : `${uid.slot}:${uid.serial}`; }
function sameUid(left, right) { return left != null && right != null && uidKey(left) === uidKey(right); }
function items() { return model.state?.items || []; }
function characters() { return model.state?.characters || []; }
function presentedCharacters() {
  const live = characters();
  if (live.length || model.state?.complete) return live;
  return (model.workspace?.roles || []).filter((role) => role.official !== false && (role.character || role.observed?.uid) && role.observed)
    .map((role) => ({ ...role.observed, uid: role.character || role.observed.uid, name: role.observed.name || role.name }));
}
function findItem(uid) { return items().find((item) => sameUid(item.uid, uid)); }
function selectedItem() { return items().find((item) => uidKey(item.uid) === model.selected); }
function activePreview() { return !model.preview?.role_id || model.preview.role_id === currentRole()?.id ? model.preview : null; }
function previewItems() { const preview = activePreview(); return Array.isArray(preview?.items) ? preview.items : items(); }
function inspectedItem() { return model.inspectPreview && model.view === "draft" ? previewItems().find((item) => uidKey(item.uid) === model.selected) : selectedItem(); }
function characterName(uid) { return characters().find((character) => sameUid(character.uid, uid))?.name || t("web.unknown_character"); }
function itemName(item) { return item?.name || item?.item_id || (item ? uidKey(item.uid) : t("web.missing_item")); }
function attributeValue(attribute) {
  return attribute.value == null ? "—" : typeof attribute.value === "number"
    ? new Intl.NumberFormat(document.documentElement.lang || "en", { ...(attribute.percent === true ? { style: "percent" } : {}), maximumFractionDigits: 4 }).format(attribute.value)
    : attribute.value;
}
function owner(item) { return item.character || null; }
function isReady() { return model.connected && model.state?.phase === "ready" && model.state?.complete === true && model.state?.items_complete === true; }
function syncPending() { return model.syncPosting || model.syncRequest != null || model.state?.sync_pending === true; }
function liveAccountReady() { return model.workspace?.live_identity_available === true && model.workspace?.live_account_matches === true; }
function hasActiveOperation() {
  const operations = model.state?.operations || [];
  return (model.pendingAction && model.operationId != null && !operations.some((operation) => String(operation.id) === String(model.operationId))) || operations.some((operation) => ["queued", "dispatched", "running"].includes(operation.status));
}
function canChange() { return model.bootstrapped && model.workspace?.account && liveAccountReady() && isReady() && !syncPending() && !model.requestBusy && !hasActiveOperation(); }
function selectedCharacter() { return presentedCharacters().find((character) => sameUid(character.uid, model.settings.selected_character)); }
function currentRole() {
  const character = selectedCharacter();
  return model.workspace?.roles?.find((role) => sameUid(role.character, character?.uid)) || null;
}
function currentEquipment() { return items().filter((item) => sameUid(owner(item), model.settings.selected_character)); }
function structuredPlan(plan) {
  const uid = (value) => value && Number.isInteger(value.slot) && Number.isInteger(value.serial);
  return Boolean(plan && uid(plan.character) && Array.isArray(plan.placements) &&
    plan.placements.every((placement) => placement && uid(placement.equipment) && Number.isInteger(placement.row) && Number.isInteger(placement.column)) &&
    (plan.core == null || uid(plan.core)));
}
function selectedDraft() { return structuredPlan(model.settings.draft) && sameUid(model.settings.draft.character, model.settings.selected_character) ? model.settings.draft : null; }
function displayPlan() { const preview = activePreview(); return preview?.readOnly ? preview.plan : selectedDraft(); }
function knownShape(item) { return item?.kind === "core" || (item?.kind === "module" && Array.isArray(item.cells) && item.cells.length > 0); }
function stagedPlans() { return Array.isArray(model.settings.draft?.plans) ? model.settings.draft.plans : []; }
function copyPlan(plan) { return structuredClone({ character: plan.character, placements: plan.placements, core: plan.core || null }); }
function setView(view) { model.view = view; if (view !== "draft" || !activePreview()?.readOnly) model.settings.filters.loadout_view = view; }
function setWorkbench(view) {
  const changed = model.settings.filters.workbench_view !== view;
  model.settings.filters.workbench_view = view;
  renderWorkbench();
  return changed;
}

function selectCharacter(uid) {
  model.settings.selected_character = uid;
  model.settings.filters.selected_role = currentRole()?.id || "";
  model.hoverAnchor = null; model.placementMode = false;
  setView(displayPlan() ? "draft" : "current"); model.inspectPreview = false;
  optimizer?.selectContext();
  avatarImages?.sync(presentedCharacters(), selectedCharacter()?.item_id);
  loadoutSlots?.sync(); pages?.sync();
  saveSoon(); render();
}

function notify(message, success = false, context = {}) {
  model.notice = { ...context, refresh: refreshSequence };
  $("notice-text").textContent = message;
  $("notice").classList.toggle("success", success);
  $("notice").dataset.kind = context.kind || (success ? "success" : "error");
  $("notice").hidden = false;
}

function isInventoryStale(error) { return ["command.stale", "inventory.stale", "workspace.stale_snapshot", "algorithm.stale_snapshot"].includes(error.code); }
function reportError(error, source = model.state?.snapshot_id) {
  notify(error.message, false, isInventoryStale(error) ? { kind: "inventory_stale", source } : error.code === "workspace.conflict" ? { kind: "workspace_stale", revision: model.workspace?.revision } : {});
}

function connection(phase) {
  $("connection").className = `connection ${phase}`;
  $("connection").textContent = t(phase === "account_mismatch" ? "web.account_mismatch_status" : phase === "identity_waiting" ? "web.identity_waiting_status" : `web.${phase}`);
}

function options(element, entries, selected) {
  const old = [...element.options].map((option) => [option.value, option.textContent]);
  if (JSON.stringify(old) !== JSON.stringify(entries)) {
    element.replaceChildren(...entries.map(([value, text]) => {
      const option = node("option", "", text);
      option.value = value;
      return option;
    }));
  }
  element.value = selected;
}

function renderHeader() {
  const state = model.state;
  const phase = model.connected ? model.workspace && !model.workspace.live_identity_available ? "identity_waiting" : model.workspace && !model.workspace.live_account_matches ? "account_mismatch" : state?.phase === "ready" && state.complete ? "ready" : state?.phase === "unavailable" ? "unavailable" : "waiting" : "disconnected";
  connection(phase);
  $("workspace-message").textContent = !model.connected ? t("web.disconnected") : state?.sync_status === "failed" ? state.message || t("web.no_inventory") :
    !state?.complete && !items().length && !characters().length && !syncPending() ? t("status.sync_required") :
    phase === "account_mismatch" ? t("web.account_mismatch") : phase === "identity_waiting" ? t("web.identity_waiting") : !isReady() ? state?.message || t("web.no_inventory") : "";
  $("workspace-message").hidden = !$("workspace-message").textContent;
  for (const id of ["search", "kind-filter", "state-filter", "owner-filter", "reset-filters"]) $(id).disabled = !model.bootstrapped || !model.workspace?.account;
  for (const button of $("kind-pills").querySelectorAll("button")) button.disabled = !model.bootstrapped || !model.workspace?.account;
  $("refresh").textContent = t(syncPending() ? "web.sync_inventory_busy" : "web.sync_inventory");
  $("refresh").disabled = !model.bootstrapped || !model.workspace?.account || model.changingAccount || syncPending();
  renderSaveState();
}

function renderWorkbench() {
  const active = ["details", "plans", "optimize"].includes(model.settings.filters.workbench_view) ? model.settings.filters.workbench_view : "optimize";
  for (const [id, panel] of [["details", "draft-panel"], ["plans", "schemes-panel"], ["optimize", "optimizer-panel"]]) {
    const selected = id === active;
    $(`${id}-tab`).classList.toggle("active", selected);
    $(`${id}-tab`).setAttribute("aria-selected", String(selected));
    $(`${id}-tab`).tabIndex = selected ? 0 : -1;
    $(panel).hidden = !selected;
  }
  if (active === "plans") loadoutSlots?.sync();
}

function renderSaveState() {
  const status = model.savePending || model.saving ? "saving" : model.settingsStatus;
  const key = { saving: "web.saving", queued: "web.settings_queued", saved: "web.saved", failed: "web.save_failed" }[status];
  $("save-state").textContent = key ? t(key) : "";
  $("retry-save").hidden = status !== "failed" || !model.workspace?.account || model.changingAccount;
}

function filteredItems() {
  const filters = model.settings.filters;
  const query = filters.search.trim().toLocaleLowerCase();
  return items().filter((item) => {
    if (query && ![item.name, item.item_id, uidKey(item.uid)].some((value) => String(value || "").toLocaleLowerCase().includes(query))) return false;
    if (filters.kind !== "all" && item.kind !== filters.kind) return false;
    if (filters.state === "locked" && !item.locked) return false;
    if (filters.state === "discarded" && !item.discarded) return false;
    if (filters.state === "normal" && (item.locked || item.discarded)) return false;
    if (filters.owner === "unequipped" && owner(item)) return false;
    if (filters.owner !== "all" && filters.owner !== "unequipped" && uidKey(owner(item)) !== filters.owner) return false;
    if (warehouse && !warehouse.matches(item)) return false;
    return true;
  }).sort((left, right) => warehouse ? warehouse.compare(left, right) : 0);
}

function syncInventoryCapacity() {
  const list = $("inventory");
  if (!list || list.clientWidth < 120 || list.clientHeight < 64) return false;
  const style = getComputedStyle(list);
  const padX = (parseFloat(style.paddingLeft) || 0) + (parseFloat(style.paddingRight) || 0);
  const padY = (parseFloat(style.paddingTop) || 0) + (parseFloat(style.paddingBottom) || 0);
  const gapX = parseFloat(style.columnGap) || 6;
  const gapY = parseFloat(style.rowGap) || 6;
  const innerW = Math.max(0, list.clientWidth - padX);
  const innerH = Math.max(0, list.clientHeight - padY);
  const minCardWidth = 286, cardHeight = 64;
  const cols = Math.max(1, Math.floor((innerW + gapX) / (minCardWidth + gapX)));
  const rows = Math.max(1, Math.floor((innerH + gapY) / (cardHeight + gapY)));
  const nextSize = cols * rows;
  let changed = false;
  if (cols !== dynamicInventoryCols) {
    dynamicInventoryCols = cols;
    list.style.gridTemplateColumns = `repeat(${cols}, minmax(0, 1fr))`;
    changed = true;
  }
  if (nextSize !== dynamicInventoryPageSize) {
    const requested = Number(model.settings.filters.inventory_page);
    const currentPage = Math.max(1, Number.isInteger(requested) ? requested : 1);
    const firstIndex = (currentPage - 1) * dynamicInventoryPageSize;
    dynamicInventoryPageSize = nextSize;
    model.settings.filters.inventory_page = String(Math.floor(firstIndex / nextSize) + 1);
    changed = true;
  }
  return changed;
}

function updateInventoryCapacity() {
  if (syncInventoryCapacity()) renderInventory();
}

function inventoryPage() {
  const inventoryPageSize = Math.max(1, dynamicInventoryPageSize);
  const all = filteredItems();
  const pages = Math.max(1, Math.ceil(all.length / inventoryPageSize));
  const requested = Number(model.settings.filters.inventory_page);
  const page = Math.min(Math.max(Number.isInteger(requested) ? requested : 1, 1), pages);
  return { all, pages, page, items: all.slice((page - 1) * inventoryPageSize, page * inventoryPageSize) };
}


function cancelPlacement() {
  const previous = model.selected;
  const restoreFocus = document.activeElement?.closest?.("#inspector-panel") || document.activeElement === boardHintContent?.cancel;
  model.placementMode = false; model.hoverAnchor = null; model.inspectPreview = false;
  model.selected = ""; boardHoverKey = ""; boardPositionChosen = false;
  renderInventory(); renderBoard(); renderInspector();
  if (restoreFocus) (inventoryRows.get(previous)?.button || [...$("board").children].find((cell) => cell.dataset.equipment === previous) || $("core-slot"))?.focus({ preventScroll: true });
}

function chooseItem(item, { place = true, toggle = false } = {}) {
  if (toggle && model.selected === uidKey(item.uid)) {
    cancelPlacement(); return;
  }
  model.selected = uidKey(item.uid);
  model.inspectPreview = false;
  model.placementMode = place && item.kind === "module";
  model.hoverAnchor = null; boardHoverKey = ""; boardPositionChosen = false;
  const anchor = firstPlacement(item);
  if (anchor) { $("row").value = String(anchor.row); $("column").value = String(anchor.column); }
  if (model.view !== "current") { setView("current"); saveSoon(); }
  renderInventory();
  renderBoard();
  renderInspector();
}

function inspectPreviewItem(item) {
  if (model.inspectPreview && model.selected === uidKey(item.uid)) { cancelPlacement(); return; }
  model.selected = uidKey(item.uid); model.inspectPreview = true; model.placementMode = false;
  renderInventory(); renderBoard(); renderInspector();
}

function renderInventory() {
  syncInventoryCapacity();
  const page = inventoryPage();
  const visible = page.items;
  $("filtered-count").textContent = t("web.filtered_count", page.all.length, items().length);
  $("inventory-page").textContent = t("web.inventory_page", page.page, page.pages);
  for (const id of ["first-page", "previous-page"]) $(id).disabled = page.page <= 1;
  for (const id of ["next-page", "last-page"]) $(id).disabled = page.page >= page.pages;
  for (const button of $("kind-pills").querySelectorAll("button")) {
    const active = button.dataset.kindValue === model.settings.filters.kind;
    button.classList.toggle("active", active);
    button.setAttribute("aria-pressed", String(active));
  }
  options($("owner-filter"), [["all", t("web.all_owners")], ["unequipped", t("web.unequipped")], ...characters().map((character) => [uidKey(character.uid), character.name || uidKey(character.uid)])], model.settings.filters.owner);
  const list = $("inventory");
  const activeKey = list.contains(document.activeElement) ? document.activeElement.dataset.focusKey : null;
  const visibleKeys = new Set(visible.map((item) => uidKey(item.uid)));
  for (const key of inventoryRows.keys()) if (!visibleKeys.has(key)) inventoryRows.delete(key);
  const rows = visible.map((item) => {
    const key = uidKey(item.uid);
    let entry = inventoryRows.get(key);
    if (!entry) {
      const row = node("div", "inventory-row"), check = node("input"), button = node("button", "inventory-item"), copy = node("span", "item-copy");
      row.setAttribute("role", "listitem"); check.type = "checkbox"; check.dataset.focusKey = `check:${key}`;
      button.type = "button"; button.dataset.focusKey = key;
      const icon = equipmentIcon(item, node); button.append(icon, copy); row.append(check, button);
      entry = { row, check, button, icon, copy, item, signature: "" }; inventoryRows.set(key, entry);
      check.addEventListener("change", () => { if (check.checked) model.checked.add(key); else model.checked.delete(key); updateSelection(); });
      button.addEventListener("click", () => chooseItem(entry.item, { toggle: true }));
    }
    entry.item = item;
    entry.row.dataset.quality = String(item.quality || "unknown").toLowerCase();
    entry.row.dataset.kind = item.kind;
    entry.row.classList.toggle("selected", model.selected === key);
    entry.check.checked = model.checked.has(key); entry.check.setAttribute("aria-label", itemName(item));
    entry.button.setAttribute("aria-pressed", String(model.selected === key));
    updateEquipmentIcon(entry.icon, item);
    const ownerLabel = owner(item) ? characterName(owner(item)) : t("web.unequipped");
    const mains = item.kind === "core"
      ? (item.main_stats_complete === true && item.main_stat?.value != null ? [item.main_stat] : [])
      : (item.main_stats || []).filter((stat) => stat?.value != null);
    const mainLabel = mains.map((stat) => `${propertyLabel(t, stat)} ${attributeValue(stat)}`).join(" · ");
    const subSummary = (item.sub_stats || []).filter((stat) => stat?.value != null).slice(0, 3)
      .map((stat) => `${propertyLabel(t, stat)} ${attributeValue(stat)}`).join(" · ");
    const details = [mainLabel, item.level == null ? "" : t("web.level", item.level), ownerLabel,
      item.locked ? t("web.locked") : "", item.discarded ? t("web.discarded") : ""].filter(Boolean);
    const fullLabel = [itemName(item), item.quality ? qualityLabel(t, item.quality) : "", ...details].filter(Boolean).join(" · ");
    entry.button.title = fullLabel; entry.button.setAttribute("aria-label", fullLabel);
    const signature = JSON.stringify([itemName(item), ownerLabel, mainLabel, subSummary, item.level, item.locked, item.discarded, item.kind, item.cells]);
    if (entry.signature !== signature) {
      entry.signature = signature;
      const head = node("span", "item-head");
      head.append(node("strong", "item-name", itemName(item)));
      if (item.level != null) head.append(node("span", "item-level", t("web.level", item.level)));
      const glyph = shapeGlyph(item, node, "mini");
      if (glyph) head.append(glyph);
      if (item.locked) head.append(node("span", "tag locked", t("web.locked")));
      if (item.discarded) head.append(node("span", "tag discarded", t("web.discarded")));
      const subline = node("span", "item-subline");
      if (mainLabel) subline.append(node("span", "item-main-stat", mainLabel));
      subline.append(node("span", "item-owner", ownerLabel));
      const children = [head, subline];
      if (subSummary) children.push(node("span", "item-subs-preview", subSummary));
      entry.copy.replaceChildren(...children);
    }
    return entry.row;
  });
  if (!rows.length) {
    const empty = node("div", "empty inventory-empty");
    empty.append(node("p", "", t(items().length ? "web.no_matches" : model.state?.complete ? "web.inventory_empty" : "web.no_inventory")));
    if (items().length) {
      const reset = node("button", "button quiet", t("web.reset_filters")); reset.type = "button";
      reset.onclick = () => $("reset-filters").click(); empty.append(reset);
    }
    rows.push(empty);
  }
  reconcileChildren(list, rows);
  if (activeKey) [...list.querySelectorAll("[data-focus-key]")].find((element) => element.dataset.focusKey === activeKey)?.focus({ preventScroll: true });
  updateSelection();
}

function updateSelection() {
  const visible = inventoryPage().items;
  const selected = visible.filter((item) => model.checked.has(uidKey(item.uid))).length;
  $("select-visible").checked = visible.length > 0 && selected === visible.length;
  $("select-visible").indeterminate = selected > 0 && selected < visible.length;
  $("select-visible").disabled = !visible.length;
  $("selected-count").textContent = t("web.selected_count", model.checked.size);
  if (model.checked.size > 512) $("selected-count").textContent += ` · ${t("web.selection_limit", 512)}`;
  $("apply-batch").disabled = !canChange() || !model.checked.size || model.checked.size > 512;
  $("batch-toolbar").hidden = !model.checked.size;
  optimizer?.render();
}

function allowedCells(character = selectedCharacter()) {
  return new Set((character?.allowed_cells || []).map(([row, column]) => `${row}:${column}`));
}

function occupiedCells(equipment, placements, character = selectedCharacter()) {
  const cells = new Map(), allowed = allowedCells(character);
  let conflict = false;
  placements.forEach((placement, index) => {
    const item = equipment.find((candidate) => sameUid(candidate.uid, placement.equipment));
    if (!item || !Array.isArray(item.cells) || !item.cells.length) return;
    item.cells.forEach(([dr, dc]) => {
      const row = placement.row + dr, column = placement.column + dc;
      if (!allowed.has(`${row}:${column}`)) { conflict = true; return; }
      const key = (row - 1) * 5 + column - 1;
      const previous = cells.get(key);
      if (previous) conflict = true;
      cells.set(key, { item, index, conflict: Boolean(previous) });
    });
  });
  return { cells, conflict };
}

function placementCheck(item, anchor, useDraft = false) {
  const allowed = allowedCells();
  if (!selectedCharacter()) return { valid: false, reason: "web.choose_character", footprint: [] };
  if (item?.kind === "core") return { valid: true, footprint: [] };
  if (!knownShape(item) || !allowed.size) return { valid: false, reason: "web.unknown_shape", footprint: [] };
  const placements = useDraft ? selectedDraft()?.placements || [] : currentEquipment().filter((value) => value.kind === "module").map((value) => ({ equipment: value.uid, row: value.row, column: value.column }));
  const occupied = occupiedCells(items(), placements.filter((value) => !sameUid(value.equipment, item.uid)));
  const footprint = item.cells.map(([dr, dc]) => [anchor.row + dr, anchor.column + dc]);
  if (footprint.some(([row, column]) => !allowed.has(`${row}:${column}`))) return { valid: false, reason: "web.placement_outside", footprint };
  if (footprint.some(([row, column]) => occupied.cells.has((row - 1) * 5 + column - 1))) return { valid: false, reason: "web.placement_occupied", footprint };
  return { valid: true, footprint };
}

function firstPlacement(item) {
  if (item?.kind !== "module" || !knownShape(item)) return null;
  if (sameUid(owner(item), model.settings.selected_character) && placementCheck(item, { row: item.row, column: item.column }).valid) return { row: item.row, column: item.column };
  for (let row = 1; row <= 5; row++) for (let column = 1; column <= 5; column++) if (placementCheck(item, { row, column }).valid) return { row, column };
  return { row: 1, column: 1 };
}

function paintFootprint() {
  const item = selectedItem();
  const anchor = model.hoverAnchor || { row: Number($("row").value), column: Number($("column").value) };
  const preview = model.placementMode && model.view === "current" && item?.kind === "module" ? placementCheck(item, anchor) : null;
  const keys = new Set((preview?.footprint || []).map(([row, column]) => `${row}:${column}`));
  for (const cell of $("board").children) {
    const within = keys.has(cell.dataset.cell);
    cell.classList.toggle("footprint", within);
    cell.classList.toggle("footprint-invalid", within && !preview.valid);
    const [row, column] = cell.dataset.cell.split(":").map(Number);
    for (const [edge, dr, dc] of [["top", -1, 0], ["right", 0, 1], ["bottom", 1, 0], ["left", 0, -1]])
      cell.style.setProperty(`--ghost-${edge}`, within && !keys.has(`${row + dr}:${column + dc}`) ? "2px" : "0px");
  }
  renderBoardHint();
}

function boardItemInstalled(item) {
  if (model.view !== "draft") return true;
  const draft = displayPlan(), actual = findItem(item.uid);
  if (!actual || !sameUid(owner(actual), draft?.character)) return false;
  if (item.kind === "core") return sameUid(draft.core, item.uid);
  const placement = draft.placements.find((value) => sameUid(value.equipment, item.uid));
  return Boolean(placement && actual.row === placement.row && actual.column === placement.column);
}

function boardPieceStatus(item) {
  return [t(boardItemInstalled(item) ? "web.already_equipped" : "web.preview_not_equipped"),
    ...(item.locked ? [t("web.locked")] : []), ...(item.discarded ? [t("web.discarded")] : [])].join(" · ");
}

function highlightBoardPiece(key = "") {
  boardHoverKey = key;
  for (const cell of $("board").children) cell.classList.toggle("piece-hover", Boolean(key) && cell.dataset.equipment === key);
  for (const [entryKey, entry] of loadoutEquipmentRows) entry.button.classList.toggle("board-hover", Boolean(key) && entryKey === key);
  renderBoardHint();
}

function renderBoardHint() {
  const hint = $("board-hint");
  if (!boardHintContent) {
    const text = node("span"), cancel = node("button", "button quiet board-cancel", t("web.board_cancel_placement"));
    text.setAttribute("role", "status"); cancel.type = "button"; cancel.onclick = cancelPlacement;
    hint.replaceChildren(text, cancel); boardHintContent = { text, cancel };
  }
  const item = selectedItem(), placing = model.placementMode && model.view === "current" && item?.kind === "module";
  let message;
  if (placing) {
    const anchor = model.hoverAnchor || { row: Number($("row").value), column: Number($("column").value) };
    const placement = placementCheck(item, anchor);
    message = !placement.valid ? t("web.board_placing_error", itemName(item), t(placement.reason)) :
      t(boardPositionChosen && !model.hoverAnchor ? "web.board_position_selected" : "web.board_placing", itemName(item));
    hint.classList.toggle("placement-error", !placement.valid);
  } else {
    const hovered = [...boardLayout.cells.values()].find((value) => uidKey(value.item.uid) === (boardHoverKey || model.selected))?.item;
    message = hovered ? t("web.board_piece_details", itemName(hovered), boardPieceStatus(hovered)) : "";
    hint.classList.remove("placement-error");
  }
  if (boardHintContent.text.textContent !== message) boardHintContent.text.textContent = message;
  boardHintContent.text.title = message;
  hint.hidden = false;
  hint.classList.toggle("is-empty", !message);
  boardHintContent.cancel.textContent = t(placing ? "web.board_cancel_placement" : "web.board_cancel_selection");
  boardHintContent.cancel.hidden = !placing && !model.selected;
  hint.classList.toggle("is-placing", placing);
}

function renderCharacters() {
  const displayed = presentedCharacters();
  options($("character-select"), [["", t("web.choose_character")], ...displayed.map((character) => [uidKey(character.uid), character.name || t("web.character")])], uidKey(model.settings.selected_character));
  const keys = new Set();
  const children = displayed.map((character) => {
    const key = character.item_id || uidKey(character.uid); keys.add(key);
    let entry = characterRows.get(key);
    if (!entry) {
      const button = node("button", "character-choice"), avatar = characterAvatar(character, node), label = node("span"); button.type = "button";
      entry = { button, avatar, label, character }; characterRows.set(key, entry);
      button.append(avatar, label); button.onclick = () => selectCharacter(entry.character.uid);
    }
    entry.character = character; entry.label.textContent = character.name || t("web.character");
    entry.button.title = entry.label.textContent; entry.button.setAttribute("aria-label", entry.label.textContent);
    updateCharacterAvatar(entry.avatar, character);
    const selected = sameUid(character.uid, model.settings.selected_character);
    entry.button.dataset.character = uidKey(character.uid); entry.button.classList.toggle("active", selected); entry.button.setAttribute("aria-pressed", String(selected));
    return entry.button;
  });
  for (const key of characterRows.keys()) if (!keys.has(key)) characterRows.delete(key);
  reconcileChildren($("character-list"), children);
  characterRail?.refreshRoster();
}

function renderBoardTelemetry(boardCharacter, layout, sourceItems, coreUid, placements) {
  const container = $("board-telemetry");
  const totalCells = boardCharacter?.allowed_cells?.length || 0;
  if (!totalCells) { container.hidden = true; container.replaceChildren(); return; }
  const usedCells = layout.cells.size;
  const suitCounts = new Map();
  const equipped = [...(coreUid ? [coreUid] : []), ...placements.map((placement) => placement.equipment)]
    .map((uid) => sourceItems.find((candidate) => sameUid(candidate.uid, uid))).filter(Boolean);
  for (const item of equipped) {
    const suit = item.suit_id ? suitLabel(t, item) : "";
    if (suit) suitCounts.set(suit, (suitCounts.get(suit) || 0) + 1);
  }
  const occupancy = node("span", "telemetry-occupancy");
  const fillBar = node("span", "telemetry-meter");
  const fill = node("i");
  fill.style.width = `${Math.min(100, Math.round((usedCells / totalCells) * 100))}%`;
  fillBar.append(fill);
  occupancy.append(fillBar, node("strong", "", `${usedCells}/${totalCells}`));
  const chips = [occupancy];
  for (const [suit, count] of [...suitCounts.entries()].sort((a, b) => b[1] - a[1]).slice(0, 2)) {
    const badge = node("span", `telemetry-set${count >= 4 ? " set-four" : count >= 2 ? " set-two" : ""}`);
    badge.append(node("span", "telemetry-set-name", suit), node("strong", "", `×${count}`));
    badge.title = `${suit} ×${count}`;
    chips.push(badge);
  }
  container.replaceChildren(...chips);
  container.hidden = false;
}

function renderLoadoutStats(sourceItems, coreUid, placements) {
  const container = $("loadout-stats");
  const equipped = [...(coreUid ? [coreUid] : []), ...placements.map((placement) => placement.equipment)]
    .map((uid) => sourceItems.find((candidate) => sameUid(candidate.uid, uid))).filter(Boolean);
  const totals = new Map();
  for (const item of equipped) {
    const stats = item.kind === "core"
      ? [...(item.main_stat ? [item.main_stat] : []), ...(item.sub_stats || [])]
      : [...(item.main_stats || []), ...(item.sub_stats || [])];
    for (const stat of stats) {
      if (typeof stat?.value !== "number" || !Number.isFinite(stat.value)) continue;
      const key = `${stat.id || stat.name || ""}|${stat.percent === true ? "p" : "f"}`;
      const existing = totals.get(key);
      if (existing) { existing.value += stat.value; existing.count += 1; }
      else totals.set(key, { id: stat.id, name: stat.name, percent: stat.percent === true, value: stat.value, count: 1 });
    }
  }
  const ranked = [...totals.values()].sort((a, b) => b.count - a.count || (b.percent ? 1 : 0) - (a.percent ? 1 : 0) || b.value - a.value).slice(0, 6);
  const pills = ranked.map((stat) => {
    const pill = node("span", "loadout-stat-pill");
    pill.append(node("span", "loadout-stat-label", propertyLabel(t, stat)), node("strong", "", attributeValue(stat)));
    return pill;
  });
  container.replaceChildren(...pills);
  container.hidden = !pills.length || Boolean(inspectedItem());
}

function renderLoadoutEquipment(sourceItems, coreUid, placements) {
  const entries = [...(coreUid ? [coreUid] : []), ...placements.map((placement) => placement.equipment)];
  const keys = new Set();
  const children = entries.map((uid) => {
    const key = uidKey(uid), item = sourceItems.find((candidate) => sameUid(candidate.uid, uid));
    keys.add(key);
    let entry = loadoutEquipmentRows.get(key);
    if (!entry) {
      const button = node("button", "loadout-equipment-item"), icon = equipmentIcon(item, node), copy = node("span", "loadout-equipment-copy");
      const head = node("span", "loadout-equipment-head"), name = node("span", "loadout-equipment-name"), glyphSlot = node("span", "loadout-equipment-glyph");
      const meta = node("span", "loadout-equipment-meta");
      button.type = "button"; head.append(name, glyphSlot); copy.append(head, meta); button.append(icon, copy);
      entry = { button, icon, name, glyphSlot, meta, item, cellsKey: "" }; loadoutEquipmentRows.set(key, entry);
      button.onclick = () => {
        if (model.view === "draft") inspectPreviewItem(entry.item);
        else chooseItem(entry.item, { place: false, toggle: true });
      };
      button.onpointerenter = () => { if (entry.item?.kind === "module") highlightBoardPiece(key); };
      button.onpointerleave = () => { if (boardHoverKey === key) highlightBoardPiece(); };
    }
    entry.item = item; updateEquipmentIcon(entry.icon, item);
    entry.name.textContent = itemName(item);
    const cellsKey = JSON.stringify([item?.kind, item?.cells]);
    if (entry.cellsKey !== cellsKey) {
      entry.cellsKey = cellsKey;
      const glyph = shapeGlyph(item, node, "mini");
      entry.glyphSlot.replaceChildren(...(glyph ? [glyph] : []));
      entry.glyphSlot.hidden = !glyph;
    }
    const metadata = item?.level == null ? [] : [t("web.level", item.level)];
    const mainStats = item?.kind === "core" ? (item.main_stat ? [item.main_stat] : []) : item?.main_stats || [];
    for (const stat of mainStats) metadata.push(`${propertyLabel(t, stat)} ${attributeValue(stat)}`);
    entry.meta.textContent = metadata.join(" · ");
    entry.button.title = [entry.name.textContent, entry.meta.textContent].filter(Boolean).join(" · ");
    entry.button.disabled = !item;
    entry.button.dataset.equipment = key;
    entry.button.dataset.kind = item?.kind || "";
    entry.button.dataset.quality = String(item?.quality || "unknown").toLowerCase();
    entry.button.classList.toggle("selected", model.selected === key);
    entry.button.classList.toggle("board-hover", Boolean(boardHoverKey) && boardHoverKey === key);
    entry.button.setAttribute("aria-pressed", String(model.selected === key));
    return entry.button;
  });
  for (const key of loadoutEquipmentRows.keys()) if (!keys.has(key)) loadoutEquipmentRows.delete(key);
  reconcileChildren($("loadout-equipment"), children);
}

function renderBoard() {
  renderCharacters();
  const character = selectedCharacter(), draft = displayPlan();
  if (model.view === "draft" && !draft) setView("current");
  const equipment = currentEquipment();
  const placements = model.view === "draft" ? draft?.placements || [] : equipment.filter((item) => item.kind === "module").map((item) => ({ equipment: item.uid, row: item.row, column: item.column }));
  const preview = activePreview();
  const boardCharacter = model.view === "draft" && Array.isArray(preview?.allowed_cells) ? { ...character, allowed_cells: preview.allowed_cells } : character;
  const layout = occupiedCells(model.view === "draft" ? previewItems() : items(), placements, boardCharacter), allowed = allowedCells(boardCharacter);
  $("loadout-character").textContent = character?.name || t("web.choose_character");
  selectedAvatar("selected-character-avatar", character, "selected");
  selectedAvatar("select-character-avatar", character, "selected small");
  for (const [id, view] of [["view-current", "current"], ["view-draft", "draft"]]) { $(id).classList.toggle("active", model.view === view); $(id).setAttribute("aria-pressed", String(model.view === view)); }
  $("view-draft").disabled = !draft;
  boardLayout = layout;
  const sourceItems = model.view === "draft" ? previewItems() : items();
  const visual = JSON.stringify([model.view, uidKey(character?.uid), [...allowed].sort(), placements.map((placement) => {
    const item = sourceItems.find((value) => sameUid(value.uid, placement.equipment));
    const actual = item ? findItem(item.uid) : null;
    return [placement, item?.kind, item?.cells, itemName(item), item?.locked, item?.discarded, item?.icon_asset_path, item?.plan_icon_asset_path, item?.icon_big_asset_path, item?.icon_small_asset_path, actual?.character, actual?.row, actual?.column];
  })]);
  const board = $("board");
  board.dataset.view = model.view;
  if (boardVisual !== visual) {
    boardVisual = visual;
    const boardCells = [];
    for (const { row, column, gridRow, gridColumn } of arrangeBoard(board, boardCharacter?.allowed_cells || [])) {
      const positionKey = (row - 1) * 5 + column - 1, occupied = layout.cells.get(positionKey);
      const cell = node("button", "board-cell"); cell.type = "button"; cell.dataset.cell = `${row}:${column}`;
      cell.style.gridRow = String(gridRow); cell.style.gridColumn = String(gridColumn);
      cell.disabled = !character && !(model.view === "draft" && draft);
      if (occupied) {
        const pieceKey = uidKey(occupied.item.uid);
        cell.dataset.equipment = pieceKey;
        cell.classList.add("occupied"); cell.classList.toggle("conflict", occupied.conflict);
        cell.style.setProperty("--piece-color", colors[occupied.index % colors.length]);
        for (const [edge, dr, dc] of [["top", -1, 0], ["right", 0, 1], ["bottom", 1, 0], ["left", 0, -1]]) {
          const neighbor = row + dr >= 1 && row + dr <= 5 && column + dc >= 1 && column + dc <= 5
            ? layout.cells.get((row + dr - 1) * 5 + column + dc - 1) : null;
          cell.style.setProperty(`--piece-${edge}`, neighbor && sameUid(neighbor.item.uid, occupied.item.uid) ? "0px" : "2px");
        }
        const label = t("web.board_piece_details", itemName(occupied.item), boardPieceStatus(occupied.item));
        cell.setAttribute("aria-label", label); cell.title = label;
        if (placements[occupied.index].row === row && placements[occupied.index].column === column) {
          const icon = equipmentIcon(occupied.item, node, "board");
          if (icon) cell.append(icon);
        }
      } else cell.setAttribute("aria-label", t("web.empty_cell", row, column));
      const point = () => {
        highlightBoardPiece(cell.dataset.equipment || "");
        if (model.placementMode) { model.hoverAnchor = { row, column }; paintFootprint(); }
      };
      cell.onpointerenter = point; cell.onfocus = point;
      cell.onpointerleave = (event) => { if (!event.relatedTarget?.closest?.("#board > .board-cell")) { model.hoverAnchor = null; highlightBoardPiece(); paintFootprint(); } };
      cell.onblur = (event) => { if (!event.relatedTarget?.closest?.(".board-cell")) { model.hoverAnchor = null; highlightBoardPiece(); paintFootprint(); } };
      cell.onclick = () => {
        const current = boardLayout.cells.get(positionKey);
        const placing = model.placementMode && selectedItem()?.kind === "module" && model.view === "current";
        if (current && !(placing && sameUid(current.item.uid, selectedItem().uid))) {
          if (model.view === "draft") inspectPreviewItem(current.item); else chooseItem(current.item, { place: false, toggle: true });
        }
        else if (placing) {
          const placement = placementCheck(selectedItem(), { row, column });
          if (!placement.valid) { model.hoverAnchor = { row, column }; paintFootprint(); return; }
          $("row").value = String(row); $("column").value = String(column); model.hoverAnchor = null; boardPositionChosen = true; paintFootprint(); renderInspector();
        }
      };
      boardCells.push(cell);
    }
    board.replaceChildren(...boardCells);
    board.onpointerleave = () => { model.hoverAnchor = null; highlightBoardPiece(document.activeElement?.closest?.(".board-cell")?.dataset.equipment || ""); paintFootprint(); };
  }
  const placingItem = model.placementMode && model.view === "current" ? selectedItem() : null;
  board.classList.toggle("is-placing", placingItem?.kind === "module");
  for (const cell of board.children) {
    const [row, column] = cell.dataset.cell.split(":").map(Number), occupied = layout.cells.get((row - 1) * 5 + column - 1);
    if (occupied) cell.dataset.change = model.view === "draft" ? boardItemInstalled(occupied.item) ? "installed" : "pending" : "current";
    else delete cell.dataset.change;
    cell.classList.toggle("selected", Boolean(occupied) && model.selected === uidKey(occupied.item.uid));
    cell.classList.toggle("placement-target", placingItem?.kind === "module" && placementCheck(placingItem, { row, column }).valid);
  }
  highlightBoardPiece(boardHoverKey);
  paintFootprint();
  const coreUid = model.view === "draft" ? draft?.core : equipment.find((item) => item.kind === "core")?.uid;
  renderBoardTelemetry(boardCharacter, layout, sourceItems, coreUid, placements);
  renderLoadoutStats(sourceItems, coreUid, placements);
  renderLoadoutEquipment(sourceItems, coreUid, placements);
  const core = (model.view === "draft" ? previewItems() : items()).find((item) => sameUid(item.uid, coreUid));
  if (!coreContent || coreContent.filled !== Boolean(core)) {
    const icon = core ? equipmentIcon(core, node, "core") : node("span", "core-icon", "◫");
    const copy = node("span", "core-copy"), label = node("span", "core-name"), stat = node("span", "core-stat");
    copy.append(label, stat);
    coreContent = { filled: Boolean(core), icon, copy, label, stat }; $("core-slot").replaceChildren(icon, copy);
  } else if (core) updateEquipmentIcon(coreContent.icon, core);
  coreContent.label.textContent = core ? itemName(core) : t("web.empty_core");
  const coreStatText = core?.main_stat?.value != null ? `${propertyLabel(t, core.main_stat)} ${attributeValue(core.main_stat)}` : "";
  coreContent.stat.textContent = coreStatText;
  coreContent.stat.hidden = !coreStatText;
  $("core-slot").dataset.quality = core ? String(core.quality || "unknown").toLowerCase() : "";
  $("core-slot").classList.toggle("filled", Boolean(core)); $("core-slot").disabled = !core; $("core-slot").onclick = core ? () => model.view === "draft" ? inspectPreviewItem(core) : chooseItem(core, { toggle: true }) : null;
  $("clear-character").hidden = model.view !== "current";
  $("clear-character").disabled = !canChange() || !character || !equipment.length;
  $("copy-current").disabled = !isReady() || !character; $("new-draft").disabled = !character;
  renderPreviewActions();
}

function renderPreviewActions() {
  const draft = displayPlan(), showing = model.view === "draft" && Boolean(draft), preview = activePreview();
  $("preview-actions").hidden = !showing;
  $("apply-draft").hidden = Boolean(preview?.readOnly); $("save-preview").hidden = Boolean(preview?.readOnly);
  $("preview-metrics").replaceChildren();
  if (!showing) return;
  if (preview?.score != null) $("preview-metrics").append(node("span", "field-label", t(preview.kind === "saved" ? "web.saved_score" : "web.optimize_score")), node("strong", "", new Intl.NumberFormat(document.documentElement.lang || "en", { maximumFractionDigits: 1 }).format(preview.score)), node("span", "meta", preview.grade || ""));
  if (preview?.readOnly) {
    $("preview-summary").textContent = t("web.software_scheme"); $("preview-summary").classList.remove("error", "warning");
    $("preview-actions").dataset.state = "software";
    $("apply-draft").textContent = t("web.software_scheme_label"); $("apply-draft").disabled = true; $("save-preview").disabled = true;
    return;
  }
  const installed = loadoutMatchesInventory(draft, model.state);
  const stale = !installed && preview?.kind === "calculation" && preview.source !== model.state?.snapshot_id;
  const problems = draftProblems(draft);
  const changed = new Set();
  for (const item of currentEquipment()) {
    const placement = draft.placements.find((value) => sameUid(value.equipment, item.uid));
    if (item.kind === "core" ? !sameUid(item.uid, draft.core) : !placement || item.row !== placement.row || item.column !== placement.column) changed.add(uidKey(item.uid));
  }
  for (const placement of draft.placements) { const item = findItem(placement.equipment); if (!sameUid(item?.character, draft.character) || item.row !== placement.row || item.column !== placement.column) changed.add(uidKey(placement.equipment)); }
  if (draft.core && !currentEquipment().some((item) => sameUid(item.uid, draft.core))) changed.add(uidKey(draft.core));
  $("preview-summary").textContent = installed ? t("web.preview_matches_current") : problems.length ? t(problems[0]) : stale ? t("web.optimize_stale_preview") : `${preview?.name ? `${preview.name} · ` : ""}${t("web.preview_changes", changed.size)}`;
  $("preview-summary").classList.toggle("error", problems.length > 0);
  $("preview-summary").classList.toggle("warning", stale && !problems.length);
  $("preview-actions").dataset.state = installed ? "installed" : problems.length ? "invalid" : stale ? "stale" : "ready";
  $("apply-draft").textContent = t("web.equip_scheme_to", selectedCharacter()?.name || t("web.character"));
  $("apply-draft").disabled = installed || !canChange() || stale || problems.length > 0;
  $("save-preview").disabled = !currentRole() || problems.length > 0 || stale;
}

function renderInspector() {
  const item = inspectedItem(), frozen = model.inspectPreview && model.view === "draft";
  $("inspector-panel").hidden = !item;
  $("loadout-equipment").hidden = Boolean(item);
  $("loadout-stats").hidden = Boolean(item) || !$("loadout-stats").childElementCount;
  $("inspector-panel").dataset.mode = frozen ? "preview" : model.placementMode ? "placing" : "inspect";
  $("inspector-panel").dataset.quality = item ? String(item.quality || "unknown").toLowerCase() : "";
  document.querySelector(".item-actions").hidden = frozen;
  document.querySelector(".state-actions").hidden = frozen;
  document.querySelector(".item-more").hidden = frozen;
  $("inspector-empty").hidden = Boolean(item);
  $("inspector").hidden = !item;
  $("item-kind").hidden = !item;
  if (!item) return;
  $("item-kind").textContent = `${t(item.kind === "core" ? "web.core" : "web.module")}${frozen ? ` · ${t("web.preview_equipment")}` : ""}`;
  $("item-name").textContent = itemName(item);
  $("item-identity").textContent = [item.item_id, uidKey(item.uid)].filter(Boolean).join(" · ");
  if (!updateEquipmentIcon($("item-shape"), item)) $("item-shape").replaceWith(Object.assign(equipmentIcon(item, node, "large"), { id: "item-shape" }));
  const inspectorGlyph = shapeGlyph(item, node, "inspector");
  const glyphWrap = $("item-shape-glyph");
  if (inspectorGlyph) {
    glyphWrap.replaceChildren(inspectorGlyph, node("span", "item-shape-cells", `${item.cells.length}`));
    glyphWrap.hidden = false;
  } else {
    glyphWrap.replaceChildren();
    glyphWrap.hidden = true;
  }
  const details = [];
  if (item.quality !== undefined && item.quality !== "") details.push(node("span", "detail-chip", qualityLabel(t, item.quality)));
  if (item.level != null) details.push(node("span", "detail-chip", t("web.level", item.level)));
  details.push(node("span", "detail-chip", owner(item) ? t("web.owner_detail", characterName(owner(item))) : t("web.unequipped")));
  if (owner(item) && item.kind === "module" && item.row && item.column) details.push(node("span", "detail-chip", t("web.position", item.row, item.column)));
  if (item.locked) details.push(node("span", "tag locked", t("web.locked")));
  if (item.discarded) details.push(node("span", "tag discarded", t("web.discarded")));
  if (!knownShape(item)) details.push(node("span", "shape-warning", t("web.unknown_shape")));
  $("item-details").replaceChildren(...details);
  const attributes = (item.kind === "core" ? [...(item.main_stat ? [{ ...item.main_stat, source: "main" }] : []), ...(item.sub_stats || []).map((stat) => ({ ...stat, source: "sub" }))] : [...(item.main_stats || []).map((stat) => ({ ...stat, source: "main" })), ...(item.sub_stats || []).map((stat) => ({ ...stat, source: "sub" }))]).map((attribute) => {
    const row = node("div", `attribute${attribute.source === "main" ? " attribute-main" : " attribute-sub"}`);
    const name = node("span", "attribute-name", propertyLabel(t, attribute));
    if (attribute.source === "main" || attribute.source === "sub") name.append(node("small", "attribute-source", t(`web.attribute_${attribute.source}`)));
    row.append(name, node("strong", "", attributeValue(attribute)));
    return row;
  });
  $("attributes").replaceChildren(...(attributes.length ? attributes : [node("p", "meta", t("web.no_attributes"))]));
  document.querySelector(".placement").classList.toggle("is-core", item.kind === "core");
  const target = selectedCharacter();
  const transferring = owner(item) && target && !sameUid(owner(item), target.uid);
  const targetCore = currentEquipment().find((candidate) => candidate.kind === "core");
  const unchanged = target && sameUid(owner(item), target.uid) && (item.kind === "core" || item.row === Number($("row").value) && item.column === Number($("column").value));
  const reposition = item.kind === "module" && target && sameUid(owner(item), target.uid) && !model.placementMode;
  $("equip-item").textContent = t(transferring ? "web.transfer_to" : item.kind === "core" && targetCore && !sameUid(targetCore.uid, item.uid) ? "web.replace_core_for" : "web.equip_item_to", target?.name || t("web.character"));
  $("unequip-item").textContent = owner(item) ? t("web.unequip_from", characterName(owner(item))) : t("web.unequip");
  $("unequip-item").hidden = !owner(item);
  const placement = placementCheck(item, { row: Number($("row").value), column: Number($("column").value) });
  $("placement-feedback").textContent = model.placementMode && item.kind === "module" && !placement.valid ? t(placement.reason) : "";
  $("placement-feedback").classList.toggle("error", item.kind === "module" && !placement.valid);
  if (reposition) $("equip-item").textContent = t("web.reposition");
  else if (unchanged && placement.valid) $("equip-item").textContent = t("web.already_equipped");
  $("equip-item").disabled = frozen || !canChange() || !target || !knownShape(item) || (!reposition && (!placement.valid || unchanged));
  $("unequip-item").disabled = frozen || !canChange() || !owner(item);
  $("add-to-draft").disabled = frozen || !target || !knownShape(item) || !placementCheck(item, { row: Number($("row").value), column: Number($("column").value) }, true).valid;
  if (frozen) { $("placement-feedback").textContent = ""; $("placement-feedback").classList.remove("error"); }
  $("placement-feedback").hidden = !$("placement-feedback").textContent;
  $("toggle-lock").textContent = t(item.locked ? "web.unlock" : "web.lock");
  $("toggle-discard").textContent = t(item.discarded ? "web.unmark_discarded" : "web.mark_discarded");
  $("toggle-lock").disabled = frozen || !canChange();
  $("toggle-discard").disabled = frozen || !canChange();
  $("assess-item").hidden = model.features.assessment !== true;
  $("assess-item").disabled = frozen || !isReady();
}

function draftProblems(draft) {
  if (!draft) return [];
  if (!structuredPlan(draft)) return ["web.invalid_saved_draft"];
  const complete = model.state?.complete === true && model.state?.items_complete === true;
  const missingCharacter = complete && !characters().some((character) => sameUid(character.uid, draft.character));
  const missing = complete && (draft.placements.some((placement) => !findItem(placement.equipment)) || (draft.core && !findItem(draft.core)));
  const unknown = draft.placements.some((placement) => { const item = findItem(placement.equipment); return item && !knownShape(item); });
  const conflict = occupiedCells(items(), draft.placements, characters().find((character) => sameUid(character.uid, draft.character))).conflict;
  return [missingCharacter ? "web.missing_draft_character" : null, missing ? "web.missing_draft_items" : null,
    unknown ? "web.unknown_shape" : null, conflict ? "web.draft_conflict" : null].filter(Boolean);
}

function renderDraft() {
  const draft = model.settings.draft;
  const content = $("draft-content");
  if (model.view === "draft" && activePreview()?.readOnly) {
    content.replaceChildren(node("p", "empty compact", t("web.software_scheme"))); $("draft-summary").textContent = ""; renderPlanGroup(); return;
  }
  if (!structuredPlan(draft)) {
    content.replaceChildren(node("div", "empty compact", t(draft ? "web.invalid_saved_draft" : "web.empty_draft")));
    $("draft-summary").textContent = "";
    renderPlanGroup();
    return;
  }
  const heading = node("div", "draft-owner");
  heading.append(node("span", "", t("web.draft_owner", characterName(draft.character))));
  if (!sameUid(draft.character, model.settings.selected_character)) {
    const select = node("button", "button quiet", t("web.view_draft_character"));
    select.type = "button";
    select.onclick = () => selectCharacter(draft.character);
    heading.append(select);
  }
  const list = node("div", "draft-list");
  const entries = [...draft.placements.map((placement, index) => ({ uid: placement.equipment, placement, index })), ...(draft.core ? [{ uid: draft.core, core: true }] : [])];
  for (const entry of entries) {
    const item = previewItems().find((value) => sameUid(value.uid, entry.uid));
    const row = node("div", "draft-row");
    row.append(equipmentIcon(item, node, "small"));
    const copy = node("div", "draft-copy");
    copy.append(node("strong", item ? "" : "missing", item ? itemName(item) : uidKey(entry.uid)));
    copy.append(node("small", "", !item ? t("web.missing_item") : entry.core ? t("web.core") : t("web.position", entry.placement.row, entry.placement.column)));
    const remove = node("button", "icon-button", "×");
    remove.type = "button";
    remove.setAttribute("aria-label", `${t("web.remove")} ${item ? itemName(item) : uidKey(entry.uid)}`);
    remove.onclick = () => { if (entry.core) draft.core = null; else draft.placements.splice(entry.index, 1); model.preview = { kind: "manual" }; saveSoon(); renderDraft(); renderBoard(); optimizer?.render(); };
    row.append(copy, remove);
    list.append(row);
  }
  content.replaceChildren(heading, list);
  if (!entries.length) content.append(node("p", "draft-warning", t("web.empty_plan_hint")));
  const problems = draftProblems(draft);
  for (const problem of problems) content.append(node("p", "draft-warning", t(problem)));
  const core = draft.core ? findItem(draft.core) : null;
  $("draft-summary").textContent = t("web.draft_summary", draft.placements.length, core ? itemName(core) : t("web.no_core"));
  renderPlanGroup();
}

function groupProblems(plans) {
  const problems = new Set(plans.flatMap((plan) => plan == null ? ["web.invalid_saved_draft"] : draftProblems(plan)));
  const assigned = new Set();
  const targets = new Set();
  for (const plan of plans.filter(structuredPlan)) {
    const target = uidKey(plan.character);
    if (targets.has(target)) problems.add("web.group_duplicate_character");
    targets.add(target);
    for (const uid of [...plan.placements.map((placement) => placement.equipment), ...(plan.core ? [plan.core] : [])]) {
      const key = uidKey(uid);
      if (assigned.has(key)) problems.add("web.group_duplicate_equipment");
      assigned.add(key);
    }
  }
  return [...problems];
}

function renderPlanGroup() {
  const plans = stagedPlans();
  const content = $("staged-plans");
  content.replaceChildren();
  for (const [index, plan] of plans.entries()) {
    const valid = structuredPlan(plan);
    const row = node("div", "group-plan");
    const copy = node("div", "draft-copy");
    copy.append(node("strong", "", valid ? characterName(plan.character) : t("web.invalid_saved_draft")));
    if (valid) {
      const core = plan.core ? findItem(plan.core) : null;
      copy.append(node("small", "", t("web.draft_summary", plan.placements.length, core ? itemName(core) : plan.core ? uidKey(plan.core) : t("web.no_core"))));
    }
    const actions = node("div", "group-plan-actions");
    const edit = node("button", "button quiet", t("web.edit_draft"));
    edit.type = "button";
    edit.disabled = !valid;
    edit.onclick = () => {
      model.settings.draft = { ...copyPlan(plan), plans };
      model.settings.selected_character = plan.character;
      model.settings.filters.selected_role = currentRole()?.id || "";
      model.preview = { kind: "manual" }; optimizer?.selectContext();
      setView("draft");
      setWorkbench("details");
      saveSoon(); render();
    };
    const remove = node("button", "button quiet", t("web.remove"));
    remove.type = "button";
    remove.onclick = () => { plans.splice(index, 1); saveSoon(); renderPlanGroup(); };
    actions.append(edit, remove);
    row.append(copy, actions);
    content.append(row);
  }
  if (!plans.length) content.append(node("div", "empty compact", t("web.empty_plan_group")));
  const problems = groupProblems(plans);
  for (const problem of problems) content.append(node("p", "draft-warning", t(problem)));
  const draft = model.settings.draft;
  $("stage-draft").disabled = !structuredPlan(draft) || draftProblems(draft).length > 0;
  $("group-summary").textContent = t("web.group_summary", plans.length, 32);
  $("apply-plans").disabled = !canChange() || !plans.length || plans.length > 32 || problems.length > 0;
}

function renderActivity() {
  const operations = model.state?.operations || [];
  const latest = model.operationId != null ? operations.find((operation) => String(operation.id) === String(model.operationId)) : model.pendingAction ? null : operations.find((operation) => ["queued", "dispatched", "running"].includes(operation.status));
  $("operation-feedback").hidden = !latest && !model.pendingAction;
  $("operation-feedback").dataset.status = latest?.status || "queued";
  const status = { confirmed: "web.operation_confirmed", failed: "web.operation_failed", unknown: "web.operation_unknown", cancelled: "web.operation_cancelled" }[latest?.status] || "web.operation_working";
  $("operation-summary").textContent = latest ? `${t(`web.${latest.action}`)} · ${t(status)}${latest.message && ["failed", "unknown", "cancelled"].includes(latest.status) ? ` · ${latest.message}` : ""}` : model.operationId != null ? t("web.operation_pending") : t("web.submitting_operation");
  const active = latest && ["queued", "dispatched", "running"].includes(latest.status);
  $("operation-progress").hidden = !active || !latest.total; $("operation-progress").max = latest?.total || 1; $("operation-progress").value = latest?.completed ?? 0;
  $("cancel").hidden = !active;
  const visual = JSON.stringify(operations);
  if (activityVisual !== visual) {
    activityVisual = visual;
    $("activity").replaceChildren(...(operations.length ? [...operations].reverse().map((operation) => {
    const row = node("div", `operation ${operation.status}`);
    const title = node("div", "operation-title", t(`web.${operation.action}`));
    if (operation.id !== undefined) title.append(node("span", "operation-id mono", operation.id));
    let status = t(`web.${operation.status}`);
    if (operation.total > 1) status += ` · ${operation.completed ?? 0}/${operation.total}`;
    row.append(title, node("div", "operation-status", status), node("div", "operation-message", operation.message || ""));
    return row;
    }) : [node("div", "empty compact", t("web.no_operations"))]));
  }
  $("cancel").disabled = !model.connected || model.requestBusy || !operations.some((operation) => ["queued", "dispatched", "running"].includes(operation.status));
}

function render() {
  renderHeader();
  renderInventory();
  renderBoard();
  renderInspector();
  renderDraft();
  renderWorkbench();
  renderActivity();
  optimizer?.render();
}

function position() {
  const row = Number($("row").value), column = Number($("column").value);
  if (!Number.isInteger(row) || row < 1 || row > 5 || !Number.isInteger(column) || column < 1 || column > 5) {
    notify(t("web.invalid_position"));
    return null;
  }
  return { row, column };
}

async function command(action, values = {}, meta = {}) {
  if (model.requestBusy || !model.connected) return;
  const application = action === "equip_one_key" || action === "apply_plans"
    ? { plans: structuredClone(action === "apply_plans" ? values.plans : [values]), preview: model.preview,
      jobId: meta.jobId || null,
      matchesPreview: (action === "apply_plans" ? values.plans : [values]).some((plan) => sameLoadout(plan, displayPlan())) }
    : null;
  model.requestBusy = true;
  if (action !== "cancel") { model.operationId = null; model.pendingAction = action; pendingLoadout = null; }
  render();
  const source = model.state.snapshot_id;
  try {
    const accepted = await api("api/command", { action, snapshot_id: source, ...values });
    if (accepted.id != null) model.operationId = accepted.id;
    if (accepted.id != null && application) pendingLoadout = { id: accepted.id, ...application };
    // Admission succeeded. A transient read failure is reflected by the
    // connection state; it must not be reported as a failed equipment change.
    try { await refresh(true); } catch { /* The next cached-state poll observes the same operation. */ }
  } catch (error) {
    if (model.operationId == null) model.pendingAction = "";
    reportError(error, source);
    if (isInventoryStale(error)) { try { await refresh(true); } catch { /* A later read keeps polling; no command is repeated. */ } }
  } finally {
    model.requestBusy = false;
    render();
  }
}

function reconcileAppliedLoadout(inventory) {
  if (!pendingLoadout || inventory.complete !== true || inventory.items_complete !== true) return false;
  const operation = inventory.operations?.find((value) => String(value.id) === String(pendingLoadout.id));
  if (!operation || ["queued", "dispatched", "running"].includes(operation.status)) return false;
  const submitted = pendingLoadout; pendingLoadout = null;
  if (operation.status !== "confirmed" || !submitted.plans.every((plan) => loadoutMatchesInventory(plan, inventory))) return false;
  if (submitted.jobId) {
    optimizationJob?.markApplied(submitted.jobId);
    notify(t("web.operation_confirmed"), true);
  }
  if (!submitted.matchesPreview || model.preview !== submitted.preview || !submitted.plans.some((plan) => sameLoadout(plan, displayPlan()))) return Boolean(submitted.jobId);
  optimizationJob?.markApplied(submitted.preview?.job_id);
  setView("current"); model.preview = null; model.inspectPreview = false;
  model.placementMode = false; model.hoverAnchor = null;
  saveSoon(); return true;
}

function saveSoon() {
  ++model.settingsEdit;
  model.savePending = true;
  model.settingsStatus = "saving";
  renderSaveState();
  clearTimeout(model.saveTimer);
  model.saveTimer = setTimeout(saveSettings, 300);
}

async function saveSettings() {
  if (!model.bootstrapped || !model.workspace?.account || model.changingAccount || model.saving || !model.savePending) return;
  const sentEdit = model.settingsEdit;
  model.saving = true;
  model.savePending = false;
  renderSaveState();
  try {
    await api("api/settings", model.settings);
    model.submittedSettingsEdit = sentEdit;
    model.settingsStatus = "queued";
  } catch (error) {
    model.settingsStatus = "failed";
    notify(error.message);
  } finally {
    model.saving = false;
    renderSaveState();
    if (model.savePending) saveSettings();
  }
}

async function readInventoryPages() {
  try { return await readInventoryCollection(); }
  catch (error) {
    if (!isInventoryStale(error)) throw error;
    // A source changed while reading pages. Start one fresh read, never mix pages.
    return readInventoryCollection();
  }
}

async function readInventoryCollection() {
  const first = await api("api/state");
  const total = first.total_items;
  if (!Array.isArray(first.items) || typeof first.snapshot_id !== "string" ||
      !Number.isSafeInteger(total) || total < first.items.length || first.page_start !== 0) {
    throw new Error(t("web.request_failed"));
  }
  if (model.state?.snapshot_id === first.snapshot_id && model.state.items_complete === true && model.state.items.length === total) {
    return { ...first, items: model.state.items, items_complete: true, next_cursor: null };
  }
  const inventory = { ...first, items: [...first.items], items_complete: false };
  let page = first;
  while (page.next_cursor !== null) {
    const cursor = page.next_cursor;
    if (!Number.isSafeInteger(cursor) || cursor !== inventory.items.length || cursor <= page.page_start) {
      throw new Error(t("web.request_failed"));
    }
    page = await api(`api/state/${encodeURIComponent(first.snapshot_id)}/${cursor}`);
    if (page.snapshot_id !== first.snapshot_id || page.total_items !== total ||
        page.page_start !== cursor || !Array.isArray(page.items) || !page.items.length) {
      throw new Error(t("web.request_failed"));
    }
    inventory.items.push(...page.items);
  }
  if (inventory.items.length !== total) throw new Error(t("web.request_failed"));
  inventory.next_cursor = null;
  inventory.items_complete = true;
  return inventory;
}

function applyRecommendedWeights(catalog) {
  if (setRecommendedWeightsCatalog(catalog)) {
    optimizer?.syncRecommendedWeights();
    pages?.sync();
  }
}

function scheduleRecommendedWeightsPoll() {
  if (recommendedWeightsTimer != null || getRecommendedWeightsCatalog().status !== "fetching" || recommendedWeightsPolls >= 30) return;
  recommendedWeightsTimer = setTimeout(async () => {
    recommendedWeightsTimer = null;
    recommendedWeightsPolls += 1;
    try {
      const latest = await api("api/recommended-weights");
      applyRecommendedWeights(latest);
      if (getRecommendedWeightsCatalog().status === "fetching") scheduleRecommendedWeightsPoll();
    } catch { /* Retain the already loaded local fallback catalog. */ }
  }, 500);
}

async function refreshRecommendedWeights() {
  if (model.features.recommended_weights !== true || !model.workspace?.account) return;
  clearTimeout(recommendedWeightsTimer); recommendedWeightsTimer = null; recommendedWeightsPolls = 0;
  try {
    const latest = await api("api/recommended-weights/refresh", {});
    applyRecommendedWeights(latest);
    if (getRecommendedWeightsCatalog().status === "fetching") scheduleRecommendedWeightsPoll();
  } catch (error) { reportError(error); }
}

async function loadBootstrap() {
  if (!model.bootstrapped) {
      const bootstrap = await api("api/bootstrap");
      Object.assign(model.messages, bootstrap.messages || {});
      model.features = bootstrap.features || {};
      if (bootstrap.recommended_weights) {
        setRecommendedWeightsCatalog(bootstrap.recommended_weights);
        if (getRecommendedWeightsCatalog().status === "fetching") {
          recommendedWeightsPolls = 0;
          scheduleRecommendedWeightsPoll();
        }
      } else {
        try {
          const response = await fetch("./recommended_weights.json", { cache: "no-store" });
          if (response.ok) setRecommendedWeightsCatalog({ ...(await response.json()), origin: "local", status: "fallback" });
        } catch { /* Built-in general weights remain available. */ }
      }
      if (bootstrap.locale) document.documentElement.lang = bootstrap.locale;
      const saved = bootstrap.settings || {};
      model.settings = { filters: { ...model.settings.filters, ...saved.filters }, selected_character: saved.selected_character ?? null, draft: saved.draft ?? null };
      setView(model.settings.filters.loadout_view === "draft" ? "draft" : "current");
      model.settings.filters.color_theme = applyPalette(model.settings.filters.color_theme);
      for (const [id, field] of [["search", "search"], ["kind-filter", "kind"], ["state-filter", "state"], ["owner-filter", "owner"]]) $(id).value = model.settings.filters[field];
      translateDocument();
      model.bootstrapped = true;
  }
}

function refresh(afterCurrent = false) {
  if (refreshPromise) return afterCurrent ? refreshPromise.catch(() => {}).then(() => refresh()) : refreshPromise;
  refreshPromise = performRefresh().finally(() => { refreshPromise = null; });
  return refreshPromise;
}

function syncInventory() {
  if (syncPromise) return syncPromise;
  if (!model.bootstrapped || !model.workspace?.account || model.changingAccount || syncPending()) return Promise.resolve();
  syncPromise = (async () => {
    const staleNotice = model.notice?.kind === "inventory_stale" ? model.notice : null;
    model.syncPosting = true; render();
    try {
      const accepted = await api("api/sync", {});
      model.syncRequest = { id: String(accepted.sync_request_id), after: refreshSequence, notice: staleNotice };
      await refresh(true);
    } catch (error) { reportError(error); }
    finally { model.syncPosting = false; render(); }
  })().finally(() => { syncPromise = null; });
  return syncPromise;
}

async function performRefresh() {
  const sequence = ++refreshSequence;
  const before = { source: model.state?.snapshot_id, phase: model.state?.phase, complete: model.state?.complete,
    connected: model.connected, canChange: Boolean(canChange()), workspace: workspaceVersion(model.workspace) };
  try {
    await loadBootstrap();
    await reloadWorkspace();
    const settingsEdit = model.settingsEdit;
    const settingsWereIdle = !model.savePending && !model.saving;
    // Publish only a complete collection from one source snapshot. A failed
    // page leaves the previous observed inventory intact.
    const inventory = await readInventoryPages();
    if (model.changingAccount) return;
    model.state = inventory;
    const appliedLoadout = reconcileAppliedLoadout(inventory);
    const syncRequest = model.syncRequest;
    const syncPublished = syncRequest && sequence > syncRequest.after && inventory.sync_pending === false;
    const syncCompleted = syncPublished && String(inventory.sync_request_id) === syncRequest.id && inventory.sync_status === "completed";
    if (syncPublished) model.syncRequest = null;
    if ((inventory.operations || []).some((operation) => String(operation.id) === String(model.operationId))) model.pendingAction = "";
    // A response started before the latest preference write cannot confirm it.
    if (settingsWereIdle && !model.savePending && !model.saving &&
        settingsEdit === model.settingsEdit && settingsEdit === model.submittedSettingsEdit &&
        ["saving", "saved", "failed"].includes(model.state.settings_status)) {
      model.settingsStatus = model.state.settings_status;
    }
    model.connected = true;
    if (inventory.phase === "ready" && inventory.complete === true && inventory.items_complete === true &&
        model.notice?.kind === "inventory_stale" && sequence > model.notice.refresh &&
        (model.notice.source == null || model.notice.source !== inventory.snapshot_id || syncCompleted && syncRequest.notice === model.notice)) {
      model.notice = null; $("notice").hidden = true;
    }
    if (model.state.complete && model.state.items_complete) {
      const existing = new Set(items().map((item) => uidKey(item.uid)));
      for (const key of model.checked) if (!existing.has(key)) model.checked.delete(key);
      if (!selectedCharacter() && model.settings.filters.selected_role) {
        const role = model.workspace?.roles?.find((value) => value.id === model.settings.filters.selected_role);
        if (role?.character && characters().some((character) => sameUid(character.uid, role.character))) model.settings.selected_character = role.character;
      }
    }
    const inventoryChanged = before.source !== inventory.snapshot_id;
    const workspaceChanged = before.workspace !== workspaceVersion(model.workspace);
    if (inventoryChanged || before.phase !== inventory.phase || before.complete !== inventory.complete) {
      optimizer?.sync(); avatarImages?.sync(presentedCharacters(), selectedCharacter()?.item_id); warehouse?.sync(); pages?.sync();
    } else {
      if (workspaceChanged) { optimizer?.sync(); pages?.sync(); }
      else optimizationJob?.refresh();
    }
    if (appliedLoadout || inventoryChanged || before.phase !== inventory.phase || before.complete !== inventory.complete ||
        before.connected !== model.connected || before.canChange !== Boolean(canChange())) render();
    else { renderHeader(); renderActivity(); if (workspaceChanged) renderWorkbench(); }
  } catch (error) {
    const changed = model.connected;
    model.connected = false;
    if (changed) render(); else renderHeader();
    throw error;
  }
}

function workspaceVersion(workspace) {
  return JSON.stringify([workspace?.account, workspace?.revision, workspace?.save_status, workspace?.live_identity_available, workspace?.live_account_matches]);
}

async function reloadWorkspace() {
  const serial = ++workspaceRead;
  const staleNotice = model.notice?.kind === "workspace_stale" ? model.notice : null;
  const workspace = await api("api/workspace");
  if (serial !== workspaceRead) return model.workspace;
  const previous = model.workspace?.account;
  if (previous && (previous.id !== workspace.account.id || previous.generation !== workspace.account.generation)) {
    model.changingAccount = true;
    location.reload();
    return workspace;
  }
  const changed = workspaceVersion(model.workspace) !== workspaceVersion(workspace);
  model.workspace = workspace;
  if (changed) { pages?.sync(); loadoutSlots?.sync(); }
  if (staleNotice && model.notice === staleNotice && staleNotice.revision !== undefined && BigInt(workspace.revision) > BigInt(staleNotice.revision)) {
    model.notice = null; $("notice").hidden = true;
  }
  if (model.savePending) saveSettings();
  return workspace;
}

async function poll() {
  try { await refresh(); } catch { /* The connection status retains the last observed snapshot. */ }
  finally { setTimeout(poll, 1500); }
}

function draftFromCurrent(empty) {
  const character = selectedCharacter();
  if (!character) return;
  const equipment = empty ? [] : currentEquipment();
  model.settings.draft = {
    character: character.uid,
    placements: equipment.filter((item) => item.kind === "module").map((item) => ({ equipment: item.uid, row: item.row, column: item.column })),
    core: equipment.find((item) => item.kind === "core")?.uid || null,
    plans: stagedPlans(),
  };
  setView("draft");
  model.preview = { kind: "manual" };
  setWorkbench("details");
  saveSoon();
  render();
}

function bindEvents() {
  const loadoutMenus = [...document.querySelectorAll(".loadout-menu")];
  document.addEventListener("pointerdown", (event) => {
    for (const menu of loadoutMenus) if (menu.open && !menu.contains(event.target)) menu.open = false;
  });
  const resumeReading = () => { if (!document.hidden) refresh().catch(() => {}); };
  window.addEventListener("focus", resumeReading); document.addEventListener("visibilitychange", resumeReading);
  document.addEventListener("keydown", (event) => {
    if (event.key !== "Escape" || event.defaultPrevented) return;
    const openMenu = loadoutMenus.find((menu) => menu.open);
    if (openMenu && !document.querySelector("dialog[open], [role='dialog'][aria-modal='true']:not([hidden])")) {
      event.preventDefault(); openMenu.open = false; openMenu.querySelector("summary").focus({ preventScroll: true }); return;
    }
    if (!model.placementMode && !model.selected) return;
    if (event.target?.isContentEditable || event.target?.closest?.("input,textarea,select,dialog,[role='dialog']") ||
        document.querySelector("dialog[open], [role='dialog'][aria-modal='true']:not([hidden])")) return;
    event.preventDefault(); cancelPlacement();
  });
  $("home-link").onclick = (event) => { event.preventDefault(); pages.go("equipment"); };
  $("manage-roles").onclick = () => pages.go("roles");
  $("inventory-rules").onclick = () => pages.openTidy();
  $("clear-item-selection").onclick = cancelPlacement;
  $("save-preview").onclick = () => { setWorkbench("plans"); saveSoon(); };
  $("dismiss-notice").onclick = () => { model.notice = null; $("notice").hidden = true; };
  $("retry-save").onclick = () => { saveSoon(); clearTimeout(model.saveTimer); saveSettings(); };
  $("refresh").onclick = syncInventory;
  for (const [id, field] of [["search", "search"], ["kind-filter", "kind"], ["state-filter", "state"], ["owner-filter", "owner"]]) {
    $(id).addEventListener(id === "search" ? "input" : "change", () => {
      model.settings.filters[field] = $(id).value;
      model.settings.filters.inventory_page = "1";
      saveSoon();
      renderInventory();
    });
  }
  for (const button of $("kind-pills").querySelectorAll("button")) {
    button.addEventListener("click", () => {
      const value = button.dataset.kindValue;
      if (!value || model.settings.filters.kind === value) return;
      model.settings.filters.kind = value;
      $("kind-filter").value = value;
      model.settings.filters.inventory_page = "1";
      saveSoon();
      renderInventory();
    });
  }
  $("character-select").onchange = () => {
    selectCharacter(characters().find((character) => uidKey(character.uid) === $("character-select").value)?.uid || null);
  };
  $("view-current").onclick = () => { setView("current"); model.inspectPreview = false; if (selectedItem()) chooseItem(selectedItem()); else { renderBoard(); renderInspector(); optimizer?.render(); } saveSoon(); renderDraft(); };
  $("view-draft").onclick = () => { setView("draft"); model.inspectPreview = true; model.placementMode = false; saveSoon(); renderBoard(); renderInspector(); renderDraft(); optimizer?.render(); };
  for (const view of ["details", "plans", "optimize"]) $(`${view}-tab`).onclick = () => { setWorkbench(view); saveSoon(); };
  document.querySelector(".workbench-tabs").addEventListener("keydown", (event) => {
    if (!["ArrowLeft", "ArrowRight", "Home", "End"].includes(event.key)) return;
    event.preventDefault();
    const views = ["optimize", "plans", "details"], index = Math.max(0, views.indexOf(model.settings.filters.workbench_view));
    const view = event.key === "Home" ? views[0] : event.key === "End" ? views[2] : views[(index + (event.key === "ArrowRight" ? 1 : 2)) % views.length];
    setWorkbench(view); saveSoon(); $(`${view}-tab`).focus();
  });
  for (const [id, destination] of [["first-page", () => 1], ["previous-page", (page) => page.page - 1], ["next-page", (page) => page.page + 1], ["last-page", (page) => page.pages]]) $(id).onclick = () => {
    const page = inventoryPage();
    model.settings.filters.inventory_page = String(destination(page));
    saveSoon(); renderInventory();
    $("inventory").scrollTop = 0;
  };
  for (const id of ["row", "column"]) $(id).oninput = () => {
    if (selectedItem()?.kind === "module" && model.view === "current") model.placementMode = true;
    boardPositionChosen = true; model.hoverAnchor = null; paintFootprint(); renderInspector();
  };
  $("select-visible").onchange = () => {
    inventoryPage().items.forEach((item) => { if ($("select-visible").checked) model.checked.add(uidKey(item.uid)); else model.checked.delete(uidKey(item.uid)); });
    renderInventory();
  };
  $("reset-filters").onclick = () => {
    Object.assign(model.settings.filters, { search: "", kind: "all", state: "all", owner: "all", inventory_page: "1" });
    for (const [id, field] of [["search", "search"], ["kind-filter", "kind"], ["state-filter", "state"], ["owner-filter", "owner"]]) $(id).value = model.settings.filters[field];
    warehouse.reset(); saveSoon(); renderInventory();
  };
  $("apply-batch").onclick = () => {
    const target = $("batch-action").value;
    const field = target === "locked" || target === "unlocked" ? "locked" : "discarded";
    const value = target === "locked" || target === "discarded";
    const operations = items().filter((item) => model.checked.has(uidKey(item.uid))).map((item) => ({ equipment: item.uid, field, value }));
    if (operations.length > 512) notify(t("web.selection_limit", 512));
    else if (operations.length) command("set_item_states", { operations });
  };
  $("equip-item").onclick = () => {
    const item = selectedItem(), character = selectedCharacter();
    if (!item || !character || !canChange()) return;
    const moving = owner(item) && !sameUid(owner(item), character.uid);
    if (item.kind === "core") {
      const targetCore = currentEquipment().find((candidate) => candidate.kind === "core");
      command(moving ? "move_core_to_character" : targetCore ? "replace_core" : "equip_core", { character: character.uid, equipment: item.uid });
    } else {
      if (sameUid(owner(item), character.uid) && !model.placementMode) {
        model.placementMode = true; model.hoverAnchor = null; boardPositionChosen = false;
        renderBoard(); renderInspector(); return;
      }
      const anchor = position();
      if (anchor && placementCheck(item, anchor).valid) command(moving ? "move_module_to_character" : "equip_module", { character: character.uid, equipment: item.uid, ...anchor });
    }
  };
  $("unequip-item").onclick = () => {
    const item = selectedItem();
    if (item && owner(item) && canChange()) command(item.kind === "core" ? "unequip_core" : "unequip_module", { character: owner(item), equipment: item.uid });
  };
  $("clear-character").onclick = () => { for (const menu of loadoutMenus) menu.open = false; if (selectedCharacter() && canChange()) command("unequip_all", { character: model.settings.selected_character }); };
  $("toggle-lock").onclick = () => { const item = selectedItem(); if (item && canChange()) command("set_item_locked", { equipment: item.uid, locked: !item.locked }); };
  $("toggle-discard").onclick = () => { const item = selectedItem(); if (item && canChange()) command("set_item_discarded", { equipment: item.uid, discarded: !item.discarded }); };
  $("assess-item").onclick = () => pages.go("assessment");
  $("copy-current").onclick = () => draftFromCurrent(false);
  $("new-draft").onclick = () => draftFromCurrent(true);
  $("add-to-draft").onclick = () => {
    const item = selectedItem(), character = selectedCharacter();
    if (!item || !character) return;
    if (!selectedDraft()) {
      if (model.settings.draft) { notify(t("web.no_draft_for_character")); return; }
      model.settings.draft = { character: character.uid, placements: [], core: null };
    }
    const draft = selectedDraft();
    if (item.kind === "core") draft.core = item.uid;
    else {
      const anchor = position();
      if (!anchor || !placementCheck(item, anchor, true).valid) return;
      const placement = draft.placements.find((entry) => sameUid(entry.equipment, item.uid));
      if (placement) Object.assign(placement, anchor);
      else draft.placements.push({ equipment: item.uid, ...anchor });
    }
    setView("draft");
    model.preview = { kind: "manual" }; setWorkbench("details");
    saveSoon(); renderDraft(); renderBoard(); optimizer?.render();
  };
  $("apply-draft").onclick = () => {
    const draft = model.settings.draft;
    const preview = activePreview();
    if (selectedDraft() && !preview?.readOnly && canChange() && !draftProblems(draft).length && !(preview?.kind === "calculation" && preview.source !== model.state?.snapshot_id)) command("equip_one_key", copyPlan(draft));
  };
  $("stage-draft").onclick = () => {
    const draft = model.settings.draft;
    if (!structuredPlan(draft) || draftProblems(draft).length) return;
    const plans = stagedPlans().map((plan) => structuredClone(plan));
    const existing = plans.findIndex((plan) => sameUid(plan?.character, draft.character));
    if (existing < 0) {
      if (plans.length >= 32) { notify(t("web.group_limit", 32)); return; }
      plans.push(copyPlan(draft));
    } else plans[existing] = copyPlan(draft);
    const settings = { ...model.settings, draft: { ...draft, plans } };
    if (new TextEncoder().encode(JSON.stringify(settings)).length > 49152) { notify(t("web.settings_too_large")); return; }
    model.settings = settings;
    saveSoon(); renderPlanGroup();
  };
  $("apply-plans").onclick = () => {
    const plans = stagedPlans();
    if (canChange() && plans.length && plans.length <= 32 && !groupProblems(plans).length) command("apply_plans", { plans: plans.map(copyPlan) });
  };
  $("cancel").onclick = () => command("cancel");
  window.addEventListener("pagehide", () => {
    if (!model.bootstrapped || !model.workspace?.account || model.changingAccount || !model.savePending) return;
    clearTimeout(model.saveTimer);
    api("api/settings", model.settings, { keepalive: true }).catch(() => {});
  });
}

async function start() {
  model.settings.filters.color_theme = applyPalette(model.settings.filters.color_theme);
  translateDocument();
  connection("connecting");
  try { await loadBootstrap(); }
  catch (error) { connection("disconnected"); notify(error.message); setTimeout(start, 1500); return; }
  optimizationJob = createOptimizationJob({ api, t });
  createSearchProgress({ mount: $("calculation-progress"), jobController: optimizationJob, t, node });
  avatarImages = createAvatarImages({ api, changed: refreshCharacterAvatars });
  const loadPlan = (plan, details = {}) => {
    const readOnly = plan.native_assemblable === false || !plan.character;
    if (!readOnly) {
      model.settings.draft = { ...copyPlan(plan), plans: stagedPlans() };
      model.settings.selected_character = plan.character;
      model.settings.filters.selected_role = currentRole()?.id || "";
    }
    const chosen = new Set([...(plan.placements || []).map((entry) => uidKey(entry.equipment)), ...(plan.core ? [uidKey(plan.core)] : [])]);
    model.preview = { kind: "saved", ...details, role_id: details.role_id || plan.role_id || currentRole()?.id,
      readOnly, ...(readOnly ? { plan: structuredClone(plan) } : {}),
      items: structuredClone(Array.isArray(details.items) ? details.items : items().filter((item) => chosen.has(uidKey(item.uid)))) };
    model.selected = uidKey(plan.core || plan.placements?.[0]?.equipment); model.inspectPreview = true;
    model.placementMode = false;
    optimizer?.selectContext();
    setView("draft"); pages?.go("equipment");
    loadoutSlots?.sync(); saveSoon(); render();
  };
  optimizer = createOptimizer({
    jobController: optimizationJob, features: () => model.features, workspace: () => model.workspace, t, node, inventory: () => model.state, selectedCharacter,
    saveSoftwarePlan: (plan) => pages.saveSoftwarePlan(plan),
    selectedUids: () => items().filter((item) => model.checked.has(uidKey(item.uid))).map((item) => item.uid),
    readPreference: () => model.settings.filters.optimizer_preferences,
    savePreference: (value) => { model.settings.filters.optimizer_preferences = value; saveSoon(); },
    readRolePreferences: () => model.settings.filters.optimizer_roles,
    saveRolePreferences: (value) => { model.settings.filters.optimizer_roles = value; saveSoon(); },
    api, reloadWorkspace, currentRole, refreshRecommendedWeights,
    canRun: canChange,
    previewPlan: () => model.view === "draft" ? displayPlan() : null,
    loadDraft: (result) => {
      loadPlan(result, { kind: "calculation", source: result.snapshot_id, job_id: optimizationJob.state().job.id, score: result.score, grade: result.grade });
    },
  });
  warehouse = createWarehouseFilters({
    mount: $("advanced-filters-root"), t, node, inventory: () => model.state,
    readPreference: () => model.settings.filters.advanced_filters,
    savePreference: (value) => { model.settings.filters.advanced_filters = value; saveSoon(); },
    changed: () => { model.settings.filters.inventory_page = "1"; renderInventory(); },
  });
  pages = createWorkspacePages({
    api, t, node, workspace: () => model.workspace, inventory: () => model.state,
    features: () => model.features, settings: () => model.settings, persist: saveSoon,
    isReady: () => isReady() && liveAccountReady(), currentRole: () => currentRole()?.id,
    getDraft: () => structuredPlan(model.settings.draft) ? copyPlan(model.settings.draft) : null,
    loadDraft: loadPlan, selectedItem, reloadWorkspace,
    selectedAccountChanged: () => { model.changingAccount = true; location.reload(); },
    jobController: optimizationJob, canRun: canChange,
    syncInventory,
    trackOperation: (id) => { model.operationId = id; model.pendingAction = "apply_warehouse_rules"; renderActivity(); },
    settingsChanged: render,
    refreshInventory: async () => { try { await refresh(true); } catch (error) { reportError(error); } },
    openOptimization: () => { setWorkbench("optimize"); saveSoon(); },
    cancelOperations: () => command("cancel"),
    inspectItem: (uid, roleId) => {
      const item = findItem(uid); if (!item) return;
      const character = characters().find((character) => (character.role_id || character.item_id) === roleId);
      if (character) selectCharacter(character.uid);
      chooseItem(item); saveSoon();
    },
    selectedUids: () => items().filter((item) => model.checked.has(uidKey(item.uid))).map((item) => item.uid),
    loadPlans: (plans) => {
      const group = plans.map(copyPlan), draft = { ...group[0], plans: group };
      const settings = { ...model.settings, draft };
      if (new TextEncoder().encode(JSON.stringify(settings)).length > 49152) { notify(t("web.settings_too_large")); return; }
      model.settings = settings; model.settings.selected_character = draft.character;
      model.settings.filters.selected_role = currentRole()?.id || ""; optimizer?.selectContext();
      model.preview = { kind: "calculation", source: optimizationJob.state().job.source_snapshot_id, job_id: optimizationJob.state().job.id };
      setView("draft"); setWorkbench("details"); pages.go("equipment"); saveSoon(); render();
    },
    applyPlans: (plans) => {
      const group = plans.map(copyPlan);
      const problems = groupProblems(group);
      if (problems.length) { notify(t(problems[0])); return; }
      if (!canChange() || !group.length || group.length > 32) return;
      command("apply_plans", { plans: group }, { jobId: optimizationJob.state().job.id });
    },
    openCalculation: (role) => {
      const character = characters().find((value) => sameUid(value.uid, role.character));
      const software = role.official === false && role.allowed_cells?.length === 20;
      if (!character && !software) { notify(t("web.roles_not_live")); return; }
      if (character) { model.settings.selected_character = character.uid; model.settings.filters.selected_role = role.id; }
      optimizer.selectRole(role); setWorkbench("optimize"); pages.go("equipment"); saveSoon(); render();
    },
  });
  loadoutSlots = createLoadoutSlots({ mount: $("loadout-slots-root"), api, t, node,
    workspace: () => model.workspace, inventory: () => model.state, role: currentRole,
    draft: () => model.view === "draft" && !activePreview()?.readOnly && selectedDraft() && !draftProblems(selectedDraft()).length ? copyPlan(selectedDraft()) : null,
    current: () => canChange() && !(model.view === "draft" && activePreview()?.kind === "calculation" && activePreview().source !== model.state?.snapshot_id),
    loadPreview: loadPlan, reloadWorkspace,
    manage: (roleId) => pages.openLibrary(roleId),
  });
  characterRail = createCharacterRail({ list: $("character-list"), previous: $("characters-previous"), next: $("characters-next") });
  const inventoryElement = $("inventory");
  inventoryResizeObserver = new ResizeObserver(() => requestAnimationFrame(updateInventoryCapacity));
  inventoryResizeObserver.observe(inventoryElement);
  window.addEventListener("resize", () => requestAnimationFrame(updateInventoryCapacity));
  inventoryElement.addEventListener("wheel", (event) => {
    if (inventoryElement.scrollHeight > inventoryElement.clientHeight + 2 || Math.abs(event.deltaY) < 16) return;
    const now = performance.now();
    if (now - inventoryWheelAt < 140) return;
    const page = inventoryPage();
    const target = event.deltaY > 0 ? Math.min(page.pages, page.page + 1) : Math.max(1, page.page - 1);
    if (target === page.page) return;
    event.preventDefault();
    inventoryWheelAt = now;
    model.settings.filters.inventory_page = String(target);
    saveSoon();
    renderInventory();
  }, { passive: false });
  bindEvents();
  updateInventoryCapacity();
  render();
  connection("connecting");
  try {
    await refresh();
  } catch (error) {
    connection("disconnected");
    notify(error.message);
  } finally {
    setTimeout(poll, 1500);
  }
}

start();
