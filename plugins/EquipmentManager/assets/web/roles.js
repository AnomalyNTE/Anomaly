import { createRoleBoard } from "./role_board.js";
import { characterAvatar } from "./asset_images.js";
import { propertyLabel } from "./equipment_display.js";
import { getRecommendedWeightsCatalog, recommendedWeightsForRole } from "./weight_preferences.js";

export function createRolesPanel({ mount, api, t, node, workspace, features, readRole, saveRole,
  calculationPreference, reloadWorkspace, openLibrary, openCalculation, openPanel }) {
  let selected = "", busy = false, error = "", dirty = false, editing = null, renderedSignature = "";
  let creation = { name: "", allowed_cells: [], locked_cells: [] };
  const heading = node("div", "page-heading");
  heading.append(node("div")); heading.firstChild.append(node("h2", "", t("web.roles_title")), node("p", "meta", t("web.roles_hint")));
  const refresh = node("button", "button quiet", t("web.refresh")); refresh.type = "button"; refresh.onclick = () => reload(); heading.append(refresh);
  const message = node("p", "page-error"); message.setAttribute("role", "alert");
  const content = node("div", "roles-layout");
  const archived = node("details", "archived-roles"), archivedList = node("div"); archived.append(node("summary", "", t("web.roles_archived")), archivedList);
  mount.append(heading, message, content, archived);
  function available() { return (workspace()?.roles || []).filter((role) => !role.archived); }
  function viewSignature() {
    const profiles = (values) => values.map(({ observed, ...profile }) => profile);
    return JSON.stringify([selected, readRole(), getRecommendedWeightsCatalog().revision, profiles(available()), profiles(workspace()?.archived_roles || [])]);
  }
  function current() { return available().find((role) => role.id === selected); }
  function button(key, action, disabled = false, type = "quiet") { const result = node("button", `button ${type}`, t(key)); result.type = "button"; result.disabled = busy || disabled; result.onclick = action; return result; }
  function render() {
    const roles = available();
    if (!roles.some((role) => role.id === selected)) selected = roles.some((role) => role.id === readRole()) ? readRole() : roles[0]?.id || "";
    refresh.disabled = busy; message.textContent = error; message.hidden = !error;
    content.replaceChildren();
    const list = node("div", "roles-list");
    for (const role of roles) {
      const entry = node("button", `role-choice${role.id === selected ? " active" : ""}`); entry.type = "button"; entry.disabled = busy;
      const copy = node("span", "role-choice-copy"); copy.append(node("strong", "", role.name), node("span", "meta", t(role.official ? "web.roles_official" : "web.roles_custom")));
      entry.append(characterAvatar(role, node), copy);
      entry.onclick = () => { if (dirty) { error = t("web.roles_unsaved"); message.textContent = error; message.hidden = false; return; } selected = role.id; saveRole(selected); render(); };
      list.append(entry);
    }
    const create = node("form", "inline-create"), createName = node("input"), createButton = node("button", "button secondary", t("web.roles_create"));
    createName.required = true; createName.maxLength = 128; createName.placeholder = t("web.roles_new_name"); createName.setAttribute("aria-label", t("web.roles_new_name"));
    createName.value = creation.name; createName.oninput = () => { creation.name = createName.value; };
    createButton.type = "submit"; createButton.disabled = busy || dirty;
    createButton.title = dirty ? t("web.roles_unsaved") : "";
    create.append(createName);
    let creationBoard;
    if (features().custom_boards === true) {
      creationBoard = createRoleBoard({ node, t, editable: true, allowedCells: creation.allowed_cells, lockedCells: creation.locked_cells,
        changed: (value) => { creation = { ...creation, ...value }; } });
      creationBoard.element.classList.add("custom-role-create-board"); create.append(creationBoard.element);
    }
    create.onsubmit = (event) => {
      event.preventDefault(); if (!create.reportValidity()) return;
      if (creationBoard && !creationBoard.valid()) { error = t("web.roles_board_required"); message.textContent = error; message.hidden = false; creationBoard.element.querySelector("button")?.focus(); return; }
      mutate({ action: "create", role: { name: createName.value, official: false, ...(creationBoard ? creationBoard.value() : {}) } });
    };
    create.append(createButton);
    const creationSection = node("details", "role-create"); creationSection.open = !roles.length;
    creationSection.append(node("summary", "", t("web.roles_create")), create); list.append(creationSection);
    const detail = node("div", "role-detail"), role = current();
    if (!role) detail.append(node("div", "empty", t("web.roles_no_profiles")));
    else {
      detail.append(node("h3", "role-detail-name", role.name));
      const shortcuts = node("div", "page-actions role-primary-actions");
      shortcuts.append(button("web.roles_open_calculation", () => openCalculation(role), dirty, "primary"), button("web.roles_open_library", () => openLibrary(role.id), dirty));
      if (features().role_panel === true && role.official !== false) shortcuts.append(button("web.roles_open_panel", () => openPanel(role.id), dirty));
      detail.append(shortcuts);
      const form = node("form", "role-form"), nameLabel = node("label", "page-field"), name = node("input"), levelLabel = node("label", "page-field"), level = node("input");
      name.value = editing?.id === role.id ? editing.name : role.name; name.required = true; name.maxLength = 128; nameLabel.append(node("span", "", t("web.roles_name")), name);
      const profileReady = features().role_panel === true && role.official !== false;
      level.type = "number"; level.min = "1"; level.step = "1"; level.value = editing?.id === role.id ? editing.level : role.profile?.character_level == null ? "" : String(role.profile.character_level); levelLabel.append(node("span", "", t("web.roles_level")), level);
      let board;
      const markDirty = () => {
        dirty = true; editing = { id: role.id, name: name.value, level: level.value, ...(board ? board.value() : {}) };
        save.disabled = busy; discard.disabled = busy; createButton.disabled = true; createButton.title = t("web.roles_unsaved");
        for (const action of [...detail.querySelectorAll(".role-primary-actions button, .role-weights-summary button, .role-management button"), ...archivedList.querySelectorAll("button")]) {
          action.disabled = true; action.title = t("web.roles_unsaved");
        }
      };
      for (const input of [name, level]) { input.disabled = busy; input.oninput = markDirty; }
      if (role.official || features().custom_boards === true) board = createRoleBoard({ node, t,
        allowedCells: editing?.id === role.id ? editing.allowed_cells || role.allowed_cells || [] : role.allowed_cells || [],
        lockedCells: editing?.id === role.id ? editing.locked_cells || role.locked_cells || [] : role.locked_cells || [],
        editable: !role.official && !busy, changed: markDirty });
      const actions = node("div", "page-actions"), save = node("button", "button primary", t("web.roles_save")); save.type = "submit"; save.disabled = busy || !dirty;
      const discard = button("web.roles_discard", () => { dirty = false; editing = null; error = ""; render(); }, !dirty);
      form.onsubmit = (event) => {
        event.preventDefault(); if (!form.reportValidity()) return;
        if (!role.official && board && !board.valid()) { error = t("web.roles_board_required"); message.textContent = error; message.hidden = false; return; }
        mutate({ action: "update", id: role.id, role: { name: name.value, ...(!role.official && board ? board.value() : {}), ...(profileReady ? { profile: { ...role.profile, character_level: level.value === "" ? null : level.valueAsNumber } } : {}) } });
      };
      actions.append(save, discard);
      form.append(nameLabel); if (profileReady) form.append(levelLabel); if (board) form.append(board.element); form.append(actions);
      const configuration = node("details", "role-configuration management-section"); configuration.open = dirty || !role.official;
      configuration.append(node("summary", "", t("web.roles_settings")), form); detail.append(configuration);
      const weights = node("div", "role-weights-summary"); weights.append(node("h3", "", t("web.optimize_weights")));
      const configured = Object.entries(role.weights || {});
      if (configured.length) {
        const list = node("dl", "weight-summary");
        for (const [id, value] of configured) list.append(node("dt", "", propertyLabel(t, id)), node("dd", "", value));
        weights.append(list);
      } else weights.append(node("p", "meta", t(Object.hasOwn(role, "weights") ? "web.roles_zero_weights" : "web.roles_no_weights")));
      const weightActions = node("div", "page-actions");
      const recommended = recommendedWeightsForRole(role);
      if (recommended) {
        weightActions.append(button("web.roles_apply_recommended", () => {
          mutate({ action: "update", id: role.id, role: { weights: recommended.weights, main_weights: recommended.main_weights } });
        }, dirty, "secondary"));
      }
      weightActions.append(button("web.roles_copy_weights", () => {
        try {
          const preferences = JSON.parse(calculationPreference() || "null");
          if (!preferences?.weights || typeof preferences.weights !== "object") throw new Error(t("web.roles_no_calculation_preferences"));
          const profile = { weights: preferences.weights, main_weights: preferences.shared_main_weights ? preferences.weights : preferences.main_weights || {},
            priority_stats: preferences.priority_stats || [], priority_equal: preferences.priority_equal === true, priority_min_grade: preferences.priority_min_grade || "A",
            require_core: preferences.require_core !== false, set_effect_mode: preferences.set_effect_mode || "four_piece", core_suit_id: preferences.core_suit_id || "", core_main_stats: preferences.core_main_stats || [],
            filters: preferences.filters || {}, property_limits: preferences.property_limits || [], allow_missing_core: preferences.allow_missing_core === true,
            include_discarded: preferences.include_discarded === true, allow_borrow: preferences.allow_borrow !== false };
          mutate({ action: "update", id: role.id, role: profile });
        } catch (failure) { error = failure.message; message.textContent = error; message.hidden = false; }
      }, dirty));
      weights.append(weightActions);
      if (recommended) weights.append(node("p", "meta", t("web.weights_attribution")));
      detail.append(weights);
      const more = node("details", "role-management management-section"), tools = node("div", "page-actions");
      tools.append(button("web.roles_archive", () => mutate({ action: "archive", id: role.id }), dirty));
      more.append(node("summary", "", t("web.more_actions")), node("p", "meta mono", role.id), tools); detail.append(more);
      if (dirty) for (const action of detail.querySelectorAll(".role-primary-actions button, .role-weights-summary button, .role-management button")) action.title = t("web.roles_unsaved");
    }
    content.append(list, detail);
    const oldRoles = workspace()?.archived_roles || []; archived.hidden = !oldRoles.length;
    archivedList.replaceChildren(...oldRoles.map((role) => {
      const row = node("div", "archived-role-row"), restore = button("web.roles_restore", () => mutate({ action: "restore", id: role.id }), dirty);
      if (dirty) restore.title = t("web.roles_unsaved"); row.append(node("span", "", role.name || role.id), restore); return row;
    }));
    renderedSignature = viewSignature();
  }
  async function reload() {
    if (busy) return;
    if (dirty) { error = t("web.roles_unsaved"); message.textContent = error; message.hidden = false; return; }
    busy = true; error = ""; render();
    try { await reloadWorkspace(); } catch (failure) { error = failure.message; }
    finally { busy = false; render(); }
  }
  async function mutate(body) {
    if (busy) return;
    busy = true; error = "";
    for (const control of mount.querySelectorAll("button,input,select")) control.disabled = true;
    try { await api("api/roles", body); await reloadWorkspace(); dirty = false; editing = null; if (body.action === "create") creation = { name: "", allowed_cells: [], locked_cells: [] }; }
    catch (failure) { error = failure.message; }
    finally { busy = false; render(); }
  }
  return { activate: render, sync() { if (!dirty && !busy && !mount.contains(document.activeElement) && renderedSignature !== viewSignature()) render(); } };
}
