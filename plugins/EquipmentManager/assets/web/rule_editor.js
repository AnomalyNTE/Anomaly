import { propertyLabel, qualityLabel } from "./equipment_display.js";
export function createRuleEditor({ rule, index, count, catalog, t, node, changed, move, remove }) {
  const card = node("section", "warehouse-rule"), heading = node("div", "warehouse-rule-heading"), enabled = node("input"), name = node("input"), action = node("select");
  enabled.type = "checkbox"; enabled.checked = rule.enabled !== false; enabled.setAttribute("aria-label", t("web.rules_enabled")); enabled.onchange = () => { rule.enabled = enabled.checked; update(); changed(); };
  name.value = rule.name || t("web.rules_number", index + 1); name.maxLength = 128; name.setAttribute("aria-label", t("web.rules_name")); name.oninput = () => { rule.name = name.value; changed(); };
  if (!rule.name) rule.name = name.value;
  for (const [value, key] of [["lock", "web.lock"], ["discard", "web.mark_discarded"], ["normal", "web.rules_restore_normal"]]) { const option = node("option", "", t(key)); option.value = value; action.append(option); } action.value = rule.action;
  action.setAttribute("aria-label", t("web.rules_action")); heading.append(enabled, name, action);
  for (const [key, handler, disabled] of [["web.cultivation_earlier", () => move(-1), index === 0], ["web.cultivation_later", () => move(1), index === count - 1], ["web.remove", remove, false]]) {
    const button = node("button", "button quiet", t(key)); button.type = "button"; button.disabled = disabled; button.dataset.orderDisabled = String(disabled); button.onclick = handler; heading.append(button);
  }
  const details = node("details", "rule-filters"); details.open = rule.editor_open === true; details.append(node("summary", "", t("web.rules_filters")));
  details.ontoggle = () => { if (rule.editor_open !== details.open) { rule.editor_open = details.open; changed(); } };
  const grid = node("div", "rule-filter-grid"), fields = {};
  function select(field, caption, values, multiple = true) {
    const label = node("label", "page-field"), input = node("select"); if (multiple) { input.multiple = true; input.size = 4; }
    const selected = rule[field] || (multiple ? [] : ""), entries = [...values];
    if (multiple) for (const id of selected) if (!entries.some(([value]) => value === id)) entries.push([id, id]);
    const property = ["main_ids", "sub_ids", "required_sub_ids"].includes(field);
    for (const [value, name] of entries) { const option = node("option", "", property ? propertyLabel(t, { id: value, name }) : name); option.value = value; option.selected = multiple ? selected.includes(value) : selected === value; input.append(option); }
    input.onchange = () => { rule[field] = multiple ? [...input.selectedOptions].map((option) => option.value) : input.value; update(); changed(); };
    label.append(node("span", "", t(caption)), input); grid.append(label); fields[field] = input; return input;
  }
  select("kinds", "web.type", [["module", t("web.module")], ["core", t("web.core")]]);
  select("qualities", "web.filter_quality", catalog.qualities.map((id) => [id, qualityLabel(t, id)]));
  const grades = [["", t("web.rules_no_grade")], ...["D", "C", "B", "A", "S", "SS", "SSS", "ACE"].map((id) => [id, id])];
  select("min_grade", "web.rules_min_grade", grades, false); select("max_grade", "web.rules_max_grade", grades, false);
  select("states", "web.state", [["normal", t("web.normal")], ["lock", t("web.locked")], ["discard", t("web.discarded")]]);
  select("role_ids", "web.rules_scoring_roles", catalog.roles.map((role) => [role.id, role.name]));
  select("main_ids", "web.filter_main", catalog.mains);
  select("sub_ids", "web.rules_sub_pool", catalog.subs); select("required_sub_ids", "web.rules_required_sub", catalog.subs);
  const matchLabel = node("label", "page-field"), match = node("input"); match.type = "number"; match.min = "1"; match.max = "4"; match.step = "1"; match.value = rule.sub_match ?? Math.min(4, Math.max(1, rule.sub_ids?.length || 1));
  match.oninput = () => { if (match.checkValidity()) { rule.sub_match = match.valueAsNumber; changed(); } }; matchLabel.append(node("span", "", t("web.rules_sub_match")), match); grid.append(matchLabel); fields.sub_match = match;
  for (const [field, key, toggle, values] of [["shape_ids", "web.rules_shape_filter", "shape_filter", catalog.shapes.map((shape) => [shape.shape_id, shape.shape_id])], ["suit_ids", "web.rules_suit_filter", "suit_filter", catalog.suits.map((suit) => [suit.id, suit.name])]]) {
    const checkLabel = node("label", "optimizer-check"), check = node("input"); check.type = "checkbox"; check.checked = rule[toggle] ?? Object.hasOwn(rule, field); rule[toggle] = check.checked;
    checkLabel.append(check, node("span", "", t(key))); grid.append(checkLabel); select(field, key, values);
    check.onchange = () => { rule[toggle] = check.checked; update(); changed(); }; fields[toggle] = check;
  }
  const skipOptions = [["normal", t("web.rules_evaluate")], ["skip", t("web.rules_skip")]];
  select("on_locked", "web.rules_on_locked", skipOptions, false); select("on_discarded", "web.rules_on_discarded", skipOptions, false);
  if (rule.on_locked == null) fields.on_locked.value = rule.action === "normal" ? "normal" : "skip";
  if (rule.on_discarded == null) fields.on_discarded.value = "normal";
  const preserveLabel = node("label", "optimizer-check"), preserve = node("input"); preserve.type = "checkbox"; preserve.checked = rule.preserve_locked === true; preserve.onchange = () => { rule.preserve_locked = preserve.checked; changed(); };
  preserveLabel.append(preserve, node("span", "", t("web.rules_preserve_locked"))); grid.append(preserveLabel);
  details.append(node("p", "meta", t("web.rules_filter_hint")), grid); card.append(heading, details);
  function update() {
    card.dataset.disabledRule = String(rule.enabled === false);
    fields.required_sub_ids.setCustomValidity(rule.enabled === false || (rule.required_sub_ids || []).every((id) => (rule.sub_ids || []).includes(id)) ? "" : t("web.rules_required_subset"));
    fields.shape_ids.dataset.filterDisabled = String(!rule.shape_filter); fields.suit_ids.dataset.filterDisabled = String(!rule.suit_filter);
    fields.sub_match.dataset.filterDisabled = String(!rule.sub_ids?.length); preserve.dataset.filterDisabled = String(rule.action !== "normal");
  }
  action.onchange = () => { rule.action = action.value; if (rule.on_locked == null) fields.on_locked.value = rule.action === "normal" ? "normal" : "skip"; update(); changed(); };
  update(); return card;
}

export function ruleRequest(rule) {
  const result = { id: rule.id, name: rule.name || "", enabled: rule.enabled !== false, action: rule.action };
  for (const field of ["kinds", "qualities", "states", "role_ids", "main_ids", "sub_ids", "required_sub_ids"]) if (rule[field]?.length) result[field] = rule[field];
  for (const field of ["min_grade", "max_grade", "on_locked", "on_discarded"]) if (rule[field]) result[field] = rule[field];
  if (rule.shape_filter) result.shape_ids = rule.shape_ids || [];
  if (rule.suit_filter) result.suit_ids = rule.suit_ids || [];
  if (rule.sub_ids?.length && rule.sub_match != null) result.sub_match = rule.sub_match;
  if (rule.preserve_locked === true) result.preserve_locked = true;
  return result;
}
