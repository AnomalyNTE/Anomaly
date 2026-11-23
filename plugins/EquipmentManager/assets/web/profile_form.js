const numeric = [
  ["character_level", "web.profile_character_level", 1], ["character_breakthrough_stage", "web.profile_character_stage", 0],
  ["fork_level", "web.profile_fork_level", 1], ["fork_breakthrough_stage", "web.profile_fork_stage", 0],
  ["fork_refinement", "web.profile_fork_refinement", 0], ["awakening_level", "web.profile_awakening", 0],
];
export function createProfileForm({ node, t, profile, changed, mode = "panel" }) {
  let externallyDisabled = false;
  const element = node("div", "profile-fields"), fields = {};
  for (const [field, key, minimum] of numeric) {
    const label = node("label", "page-field"), input = node("input"); input.type = "number"; input.min = minimum; input.step = "1"; input.value = profile[field] == null ? "" : profile[field];
    label.append(node("span", "", t(key)), input); element.append(label); fields[field] = input; input.oninput = changed;
  }
  const forkLabel = node("label", "page-field"), forkMode = node("select"), fork = node("input"); fork.type = "text"; fork.value = profile.fork_id || ""; fork.setAttribute("aria-label", t("web.profile_fork_id"));
  for (const [value, key] of [["unknown", "web.profile_unset"], ["none", "web.profile_no_fork"], ["id", "web.profile_fork_by_id"]]) { const option = node("option", "", t(key)); option.value = value; forkMode.append(option); }
  forkMode.value = profile.fork_id == null ? "unknown" : profile.fork_id === "" ? "none" : "id";
  function forkState() { fork.hidden = forkMode.value !== "id"; fork.required = forkMode.value === "id"; }
  forkMode.onchange = () => { forkState(); applyDisabled(); changed(); }; forkState();
  forkLabel.append(node("span", "", t("web.profile_fork_choice")), forkMode, fork); element.append(forkLabel); fields.fork_id = fork; fork.oninput = changed;
  const affinityLabel = node("label", "page-field"), affinity = node("select");
  for (const [value, key] of [["", "web.profile_unset"], ["yes", "web.profile_affinity_yes"], ["no", "web.profile_affinity_no"]]) { const option = node("option", "", t(key)); option.value = value; affinity.append(option); }
  affinity.value = profile.likeability_10 == null ? "" : profile.likeability_10 ? "yes" : "no";
  affinityLabel.append(node("span", "", t("web.profile_affinity")), affinity); if (mode === "panel") { element.append(affinityLabel); fields.likeability_10 = affinity; } affinity.onchange = changed;
  const awakeningsLabel = node("label", "page-field profile-wide"), awakeningMode = node("select"), awakenings = node("textarea"); awakenings.rows = 3; awakenings.value = profile.enabled_awakening_ids?.join("\n") || "";
  for (const [value, key] of [["unknown", "web.profile_unset"], ["none", "web.profile_no_awakening_effects"], ["ids", "web.profile_awakening_by_id"]]) { const option = node("option", "", t(key)); option.value = value; awakeningMode.append(option); }
  awakeningMode.value = profile.enabled_awakening_ids == null ? "unknown" : profile.enabled_awakening_ids.length ? "ids" : "none";
  awakeningsLabel.append(node("span", "", t("web.profile_awakening_ids")), awakeningMode, awakenings, node("small", "meta", t("web.profile_ids_hint"))); if (mode === "panel") element.append(awakeningsLabel); awakenings.oninput = changed;
  awakeningMode.onchange = () => { applyDisabled(); changed(); };
  const furniture = {};
  for (const [field, key, percent] of [["AtkAdd", "web.profile_furniture_atk", false], ["CritDamageBase", "web.profile_furniture_crit", true]]) {
    const label = node("label", "page-field"), input = node("input"); input.type = "number"; input.step = "any"; input.min = "0"; input.value = profile.furniture?.[field] == null ? "" : profile.furniture[field] * (percent ? 100 : 1);
    label.append(node("span", "", t(key)), input); if (mode === "panel") element.append(label); furniture[field] = input; input.oninput = changed;
  }
  const skills = node("section", "profile-wide profile-skills"), skillMode = node("select"); skills.append(node("h4", "", t("web.profile_skills")));
  for (const [value, key] of [["unknown", "web.profile_unset"], ["none", "web.profile_no_skills"], ["levels", "web.profile_skills_by_id"]]) { const option = node("option", "", t(key)); option.value = value; skillMode.append(option); }
  skillMode.setAttribute("aria-label", t("web.profile_skills")); skillMode.value = profile.skill_levels == null ? "unknown" : Object.keys(profile.skill_levels).length ? "levels" : "none";
  const rows = node("div", "skill-level-rows"), entries = Object.entries(profile.skill_levels || {}).map(([id, level]) => ({ id, level }));
  const add = node("button", "button quiet", t("web.profile_add_skill")); add.type = "button"; add.onclick = () => { entries.push({ id: "", level: "" }); renderSkills(); changed(); };
  skills.append(skillMode, rows, add); element.append(skills); skillMode.onchange = () => { applyDisabled(); changed(); };
  function renderSkills() {
    rows.replaceChildren();
    for (const [index, entry] of entries.entries()) {
      const row = node("div", "skill-level-row"), id = node("input"), level = node("input"), remove = node("button", "button quiet danger", t("web.remove"));
      id.value = entry.id; id.required = true; id.setAttribute("aria-label", t("web.profile_skill_id"));
      level.type = "number"; level.min = "0"; level.step = "1"; level.required = true; level.value = entry.level; level.setAttribute("aria-label", t("web.profile_skill_level"));
      id.oninput = () => { entry.id = id.value; changed(); }; level.oninput = () => { entry.level = level.value; changed(); };
      remove.type = "button"; remove.onclick = () => { entries.splice(index, 1); renderSkills(); changed(); }; row.append(id, level, remove); rows.append(row);
    }
  }
  function applyDisabled() {
    for (const control of element.querySelectorAll("input,select,textarea,button")) control.disabled = externallyDisabled;
    fork.disabled = externallyDisabled || forkMode.value !== "id";
    if (forkMode.value === "none") for (const field of ["fork_level", "fork_breakthrough_stage", "fork_refinement"]) fields[field].disabled = true;
    awakenings.hidden = awakeningMode.value !== "ids"; awakenings.required = awakeningMode.value === "ids"; awakenings.disabled = externallyDisabled || awakeningMode.value !== "ids";
    rows.hidden = skillMode.value !== "levels"; add.hidden = skillMode.value !== "levels";
    for (const control of rows.querySelectorAll("input,button")) control.disabled = externallyDisabled || skillMode.value !== "levels";
  }
  renderSkills(); applyDisabled();
  return {
    element,
    setDisabled(value) { externallyDisabled = value; applyDisabled(); },
    value() {
      if (skillMode.value === "levels" && new Set(entries.map((entry) => entry.id)).size !== entries.length) throw new Error(t("web.profile_duplicate_skills"));
      if (skillMode.value === "levels" && !entries.length) throw new Error(t("web.profile_skill_required"));
      const result = mode === "panel" ? { ...profile } : {};
      for (const [field, input] of Object.entries(fields)) result[field] = field === "fork_id" ? forkMode.value === "none" ? "" : forkMode.value === "id" ? input.value : null : field === "likeability_10" ? input.value === "" ? null : input.value === "yes" : input.value === "" ? null : input.valueAsNumber;
      if (mode === "panel") result.enabled_awakening_ids = awakeningMode.value === "none" ? [] : awakeningMode.value === "ids" ? awakenings.value.split(/\r?\n/).map((value) => value.trim()).filter(Boolean) : null;
      result.skill_levels = skillMode.value === "none" ? {} : skillMode.value === "levels" ? Object.fromEntries(entries.map((entry) => [entry.id, Number(entry.level)])) : null;
      if (mode === "panel") result.furniture = { ...profile.furniture, AtkAdd: furniture.AtkAdd.value === "" ? null : furniture.AtkAdd.valueAsNumber, CritDamageBase: furniture.CritDamageBase.value === "" ? null : furniture.CritDamageBase.valueAsNumber / 100 };
      return result;
    },
  };
}
