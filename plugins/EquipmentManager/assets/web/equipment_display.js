export function qualityLabel(t, value) {
  // Preserve the formal quality ID in requests; translate only its label.
  const key = `web.quality_${value.toLowerCase()}`;
  const label = t(key);
  return label === key ? value : label;
}

// Exact property IDs from the reference's property choices and panel labels.
// This is a display vocabulary; IDs and percentage units stay authoritative.
const namedProperties = new Set([
  "AtkBase", "AtkUp", "AtkAdd", "DefBase", "DefUp", "DefAdd",
  "HPMaxBase", "HPMaxUp", "HPMaxAdd", "CritBase", "CritDamageBase",
  "DamageUpGeneralBase", "MagBase", "UnbalIntensityBase", "HealUp", "HealBeUp",
  "ChargeGetEfficiencyBase", "DamageUpCosmosBase", "DamageUpNatureBase",
  "DamageUpIncantationBase", "DamageUpChaosBase", "DamageUpPsycheBase",
  "DamageUpLakshanaBase", "DamageUpPsychicallyBase",
]);

export function propertyLabel(t, value) {
  const id = typeof value === "string" ? value : value?.id || "";
  const name = typeof value?.name === "string" ? value.name.trim() : "";
  if (name && name !== id) return name;
  if (namedProperties.has(id)) {
    const key = `web.preference_stat_${id.toLowerCase()}`;
    const label = t(key);
    if (label && label !== key) return label;
  }
  return name || id;
}

export function suitLabel(t, item) {
  const name = item?.suit_name || (item?.kind === "core" && item.name !== item.item_id ? item.name : "");
  return name && name !== item?.suit_id && !/^Suit\d+$/i.test(name) ? name : t("web.unknown_suit");
}
