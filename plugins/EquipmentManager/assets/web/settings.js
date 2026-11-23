import { palettes, applyPalette } from "./appearance.js";

const defaults = { enabled: true, bindings: { refresh: "Alt+R", search: "Alt+F", equipment: "Alt+1", library: "Alt+2", roles: "Alt+3", optimize: "Alt+4", multi: "Alt+5" } };
export function createSettingsPanel({ mount, t, node, settings, persist, changed, actions, features }) {
  let preferences = structuredClone(defaults), loaded = false, error = "";
  const heading = node("div", "page-heading"); heading.append(node("div")); heading.firstChild.append(node("h2", "", t("web.settings_title")), node("p", "meta", t("web.settings_hint")));
  const content = node("div", "settings-content"), appearance = node("section", "settings-appearance management-section");
  const themeLabel = node("fieldset", "theme-options"), themeGrid = node("div", "theme-grid"), themes = new Map();
  themeLabel.append(node("legend", "", t("web.color_theme")), themeGrid);
  for (const [id, palette] of Object.entries(palettes)) {
    const label = node("label", "theme-option"), input = node("input"), preview = node("span", "theme-preview");
    input.type = "radio"; input.name = "equipment-color-theme"; input.value = id;
    preview.setAttribute("aria-hidden", "true"); preview.style.background = palette.colors["--canvas"];
    for (const token of ["--surface", "--accent", "--ink"]) { const chip = node("i"); chip.style.background = palette.colors[token]; preview.append(chip); }
    input.onchange = () => { if (input.checked) { settings().filters.color_theme = applyPalette(id); persist(); changed(); render(); } };
    label.append(input, preview, node("span", "", t(`web.theme_${id}`))); themeGrid.append(label); themes.set(id, { input, label });
  }
  const sizeLabel = node("label", "page-field"), size = node("select");
  sizeLabel.hidden = true;
  for (const value of [20, 40, 80]) { const option = node("option", "", t("web.settings_page_size_value", value)); option.value = String(value); size.append(option); }
  sizeLabel.append(node("span", "", t("web.settings_page_size")), size);
  size.onchange = () => { settings().filters.inventory_page_size = size.value; settings().filters.inventory_page = "1"; persist(); changed(); };
  appearance.append(themeLabel, sizeLabel);
  const shortcuts = node("details", "shortcut-settings management-section"); shortcuts.append(node("summary", "", t("web.shortcuts_title")), node("p", "meta", t("web.shortcuts_hint")));
  const enabledLabel = node("label", "optimizer-check"), enabled = node("input"); enabled.type = "checkbox";
  enabledLabel.append(enabled, node("span", "", t("web.shortcuts_enabled"))); enabled.onchange = () => { preferences.enabled = enabled.checked; save(); };
  const rows = node("div", "shortcut-rows"), inputs = new Map();
  for (const action of actions) {
    const row = node("label", "shortcut-row"), input = node("input"); input.type = "text"; input.readOnly = true; input.setAttribute("aria-label", t(action.label));
    input.onkeydown = (event) => {
      event.preventDefault(); event.stopPropagation();
      if (["Control", "Alt", "Shift", "Meta", "OS"].includes(event.key)) return;
      if (event.key === "Escape") { input.blur(); return; }
      const binding = ["Delete", "Backspace"].includes(event.key) && !event.ctrlKey && !event.altKey && !event.metaKey ? "" : chord(event);
      if (binding && !event.ctrlKey && !event.altKey && !event.metaKey) { error = t("web.shortcuts_modifier_required"); render(); return; }
      if (binding && Object.entries(preferences.bindings).some(([id, value]) => id !== action.id && value === binding)) { error = t("web.shortcuts_conflict"); render(); return; }
      preferences.bindings[action.id] = binding; save();
    };
    inputs.set(action.id, input); row.append(node("span", "", t(action.label)), input); rows.append(row);
  }
  const reset = node("button", "button quiet", t("web.shortcuts_reset")); reset.type = "button"; reset.onclick = () => { preferences.bindings = { ...defaults.bindings }; save(); };
  const notice = node("p", "settings-error"); notice.setAttribute("role", "alert");
  shortcuts.append(enabledLabel, rows, reset); content.append(appearance, shortcuts, notice); mount.append(heading, content);
  function chord(event) { return [event.ctrlKey ? "Ctrl" : "", event.altKey ? "Alt" : "", event.shiftKey ? "Shift" : "", event.metaKey ? "Meta" : "", event.key.length === 1 ? event.key.toUpperCase() : event.key].filter(Boolean).join("+"); }
  function save() { error = ""; settings().filters.web_shortcuts = JSON.stringify(preferences); persist(); render(); }
  function initialize() {
    try {
      const saved = JSON.parse(settings().filters.web_shortcuts || "{}");
      if (!saved || typeof saved !== "object" || Array.isArray(saved) || (Object.hasOwn(saved, "enabled") && typeof saved.enabled !== "boolean") || (Object.hasOwn(saved, "bindings") && (!saved.bindings || typeof saved.bindings !== "object" || Array.isArray(saved.bindings) || Object.values(saved.bindings).some((value) => typeof value !== "string")))) throw new Error();
      preferences = { ...preferences, ...saved, bindings: { ...preferences.bindings, ...saved.bindings } };
    } catch { error = t("web.shortcuts_invalid_preferences"); }
    loaded = true;
  }
  function render() {
    for (const [id, { input, label }] of themes) { input.checked = id === settings().filters.color_theme; label.classList.toggle("selected", input.checked); }
    size.value = ["20", "40", "80"].includes(settings().filters.inventory_page_size) ? settings().filters.inventory_page_size : "40";
    enabled.checked = preferences.enabled;
    for (const action of actions) { const input = inputs.get(action.id); input.value = preferences.bindings[action.id] || ""; input.placeholder = t("web.shortcuts_unassigned"); input.closest("label").hidden = Boolean(action.feature) && features()[action.feature] !== true; }
    notice.textContent = error; notice.hidden = !error;
  }
  document.addEventListener("keydown", (event) => {
    if (!loaded || !preferences.enabled || event.repeat || event.defaultPrevented || event.target.closest("input,select,textarea,[contenteditable='true']")) return;
    const binding = chord(event), action = actions.find((action) => preferences.bindings[action.id] === binding && (!action.feature || features()[action.feature] === true));
    if (!action) return; event.preventDefault(); action.run();
  });
  return { activate() { if (!loaded) initialize(); render(); }, sync() { if (!loaded) initialize(); render(); } };
}
